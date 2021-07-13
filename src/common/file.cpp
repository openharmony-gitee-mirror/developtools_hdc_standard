/*
 * Copyright (C) 2021 Huawei Device Co., Ltd.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "file.h"
#include "serial_struct.h"

namespace Hdc {
HdcFile::HdcFile(HTaskInfo hTaskInfo)
    : HdcTransferBase(hTaskInfo)
{
    commandBegin = CMD_FILE_BEGIN;
    commandData = CMD_FILE_DATA;
}

HdcFile::~HdcFile()
{
    WRITE_LOG(LOG_DEBUG, "~HdcFile");
};

void HdcFile::StopTask()
{
    WRITE_LOG(LOG_DEBUG, "HdcFile StopTask");
    singalStop = true;
};

// Send supported below styles
// send|recv path/filename path/filename
// send|recv filename  path
bool HdcFile::BeginTransfer(CtxFile *context, const char *command)
{
    int argc = 0;
    int srcOffset = 0;
    bool ret = false;
    const string CMD_OPTION_TSTMP = "-a";
    const string CMD_OPTION_SYNC = "-sync";
    const string CMD_OPTION_ZIP = "-z";
    char **argv = Base::SplitCommandToArgs(command, &argc);
    if (argc < CMD_ARG1_COUNT) {
        goto Finish;
    }
    context->localPath = argv[argc - 2];
    for (int i = 0; i < argc - CMD_ARG1_COUNT; i++) {
        if (argv[i] == CMD_OPTION_ZIP) {
            context->transferConfig.compressType = COMPRESS_LZ4;
            srcOffset += strlen(argv[i]) + 1;
        } else if (argv[i] == CMD_OPTION_SYNC) {
            context->transferConfig.updateIfNew = true;
            srcOffset += strlen(argv[i]) + 1;
        } else if (argv[i] == CMD_OPTION_TSTMP) {
            context->transferConfig.holdTimestamp = true;
            srcOffset += strlen(argv[i]) + 1;
        }
    }
    context->remotePath = argv[argc - 1];
    if (argc > CMD_ARG1_COUNT) {
        context->localPath
            = std::string(command + srcOffset, strlen(command) - srcOffset - context->remotePath.size() - 1);
    } else {
        context->localPath = argv[0];
    }
    if (!Base::CheckDirectoryOrPath(context->localPath.c_str(), true, true)) {
        goto Finish;
    }
    context->localName = Base::GetFullFilePath(context->localPath);
    ++refCount;
    uv_fs_open(loopTask, &context->fsOpenReq, context->localPath.c_str(), O_RDONLY, 0, OnFileOpen);
    context->master = true;
    ret = true;
Finish:
    if (!ret) {
        LogMsg(MSG_FAIL, "Transfer master path failed");
    }
    if (argv) {
        delete[]((char *)argv);
    }
    return ret;
}

void HdcFile::CheckMaster(CtxFile *context)
{
    string s = SerialStruct::SerializeToString(context->transferConfig);
    SendToAnother(CMD_FILE_CHECK, (uint8_t *)s.c_str(), s.size());
}

void HdcFile::WhenTransferFinish(CtxFile *context)
{
    WRITE_LOG(LOG_DEBUG, "HdcTransferBase OnFileClose");
    uint8_t flag = 1;
    SendToAnother(CMD_FILE_FINISH, &flag, 1);
}

void HdcFile::TransferSummary(CtxFile *context)
{
    uint64_t nMSec = Base::GetRuntimeMSec() - context->transferBegin;
    double fRate = static_cast<double>(context->indexIO) / nMSec;  // / /1000 * 1000 = 0
    LogMsg(MSG_OK, "FileTransfer finish, Size:%lld time:%lldms rate:%lfkB/s", context->indexIO, nMSec, fRate);
}

bool HdcFile::SlaveCheck(uint8_t *payload, const int payloadSize)
{
    bool ret = true;
    bool childRet = false;
    // parse option
    string serialStrring((char *)payload, payloadSize);
    TransferConfig &stat = ctxNow.transferConfig;
    SerialStruct::ParseFromString(stat, serialStrring);
    ctxNow.fileSize = stat.fileSize;
    ctxNow.localPath = stat.path;
    ctxNow.master = false;
    ctxNow.fsOpenReq.data = &ctxNow;
    // check path
    childRet = SmartSlavePath(ctxNow.localPath, stat.optionalName.c_str());
    if (childRet && ctxNow.transferConfig.updateIfNew) {  // file exist and option need update
        // if is newer
        uv_fs_t fs;
        Base::ZeroStruct(fs.statbuf);
        uv_fs_stat(nullptr, &fs, ctxNow.localPath.c_str(), nullptr);
        uv_fs_req_cleanup(&fs);
        if ((uint64_t)fs.statbuf.st_mtim.tv_sec >= ctxNow.transferConfig.mtime) {
            LogMsg(MSG_FAIL, "Target file is the same date or newer,path: %s", ctxNow.localPath.c_str());
            return false;
        }
    }
    // begin work
    ++refCount;
    uv_fs_open(loopTask, &ctxNow.fsOpenReq, ctxNow.localPath.c_str(), UV_FS_O_TRUNC | UV_FS_O_CREAT | UV_FS_O_WRONLY,
               S_IWUSR | S_IRUSR, OnFileOpen);
    ctxNow.transferBegin = Base::GetRuntimeMSec();
    return ret;
}

bool HdcFile::CommandDispatch(const uint16_t command, uint8_t *payload, const int payloadSize)
{
    HdcTransferBase::CommandDispatch(command, payload, payloadSize);
    bool ret = true;
    switch (command) {
        case CMD_FILE_INIT: {  // initial
            ret = BeginTransfer(&ctxNow, (char *)payload);
            ctxNow.transferBegin = Base::GetRuntimeMSec();
            break;
        }
        case CMD_FILE_CHECK: {
            ret = SlaveCheck(payload, payloadSize);
            break;
        }
        case CMD_FILE_FINISH: {
            if (*payload) {  // close-step3
                --(*payload);
                SendToAnother(CMD_FILE_FINISH, payload, 1);
                ++refCount;
                uv_fs_close(loopTask, &ctxNow.fsCloseReq, ctxNow.fsOpenReq.result, OnFileClose);
            } else {  // close-step3
                TransferSummary(&ctxNow);
                TaskFinish();
            }
            break;
        }
        default:
            break;
    }
    return ret;
}
}  // namespace Hdc