/*
 * Copyright (C) 2022 Huawei Device Co., Ltd.
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

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <string.h>
#include <limits.h>
#include <libgen.h>
#include <sys/types.h>
#include <sys/stat.h>
#include "securec.h"
#include "endian_internal.h"
#include "cJSON.h"
#include "syscap_define.h"
#include "syscap_tool.h"

typedef struct ProductCompatibilityIDHead {
    uint16_t apiVersion : 15;
    uint16_t apiVersionType : 1;
    uint16_t systemType : 3;
    uint16_t reserved : 13;
    uint32_t manufacturerID;
} PCIDHead;

typedef struct RequiredProductCompatibilityIDHead {
    uint16_t apiVersion : 15;
    uint16_t apiVersionType : 1;
} RPCIDHead;

#define SINGLE_FEAT_LENGTH  128
#define UINT8_BIT 8
#define RPCID_OUT_BUFFER 32
#define BYTES_OF_OS_SYSCAP 120
#define U32_TO_STR_MAX_LEN 11

#define PRINT_ERR(...) \
    do { \
        printf("ERROR: in file %s at line %d -> ", __FILE__, __LINE__); \
        printf(__VA_ARGS__); \
    } while (0)

static void FreeContextBuffer(char *contextBuffer)
{
    (void)free(contextBuffer);
}

static int32_t GetFileContext(char *inputFile, char **contextBufPtr, uint32_t *bufferLen)
{
    int32_t ret;
    FILE *fp = NULL;
    struct stat statBuf;
    char *contextBuffer = NULL;
    char path[PATH_MAX + 1] = {0x00};

#ifdef _POSIX_
    if (strlen(inputFile) > PATH_MAX || strncpy_s(path, PATH_MAX, inputFile, strlen(inputFile)) != EOK) {
        PRINT_ERR("get path(%s) failed\n", inputFile);
        return -1;
    }
#else
    if (strlen(inputFile) > PATH_MAX || realpath(inputFile, path) == NULL) {
        PRINT_ERR("get file(%s) real path failed\n", inputFile);
        return -1;
    }
#endif

    ret = stat(path, &statBuf);
    if (ret != 0) {
        PRINT_ERR("get file(%s) st_mode failed, errno = %d\n", path, errno);
        return -1;
    }
    if (!(statBuf.st_mode & S_IRUSR)) {
        PRINT_ERR("don't have permission to read the file(%s)\n", path);
        return -1;
    }
    contextBuffer = (char *)malloc(statBuf.st_size + 1);
    if (contextBuffer == NULL) {
        PRINT_ERR("malloc buffer failed, size = %d, errno = %d\n", (int32_t)statBuf.st_size + 1, errno);
        return -1;
    }
    fp = fopen(path, "rb");
    if (fp == NULL) {
        PRINT_ERR("open file(%s) failed, errno = %d\n", path, errno);
        FreeContextBuffer(contextBuffer);
        return -1;
    }
    size_t retFread = fread(contextBuffer, statBuf.st_size, 1, fp);
    if (retFread != 1) {
        PRINT_ERR("read file(%s) failed, errno = %d\n", path, errno);
        FreeContextBuffer(contextBuffer);
        (void)fclose(fp);
        return -1;
    }
    contextBuffer[statBuf.st_size] = '\0';
    (void)fclose(fp);

    *contextBufPtr = contextBuffer;
    *bufferLen = statBuf.st_size + 1;
    return 0;
}

static int32_t ConvertedContextSaveAsFile(char *outDirPath, const char *filename,
                                          char *convertedBuffer, uint32_t bufferLen)
{
    int32_t ret;
    FILE *fp = NULL;
    char path[PATH_MAX + 1] = {0x00};

#ifdef _POSIX_
    if (strlen(outDirPath) > PATH_MAX || strncpy_s(path, PATH_MAX, outDirPath, strlen(outDirPath)) != EOK) {
        PRINT_ERR("get path(%s) failed\n", outDirPath);
        return -1;
    }
#else
    if (strlen(outDirPath) > PATH_MAX || realpath(outDirPath, path) == NULL) {
        PRINT_ERR("get file(%s) real path failed\n", outDirPath);
        return -1;
    }
#endif

    int32_t pathLen = strlen(path);
    if (path[pathLen - 1] != '/' && path[pathLen - 1] != '\\') {
        path[pathLen] = '/';
    }

    if (strlen(filename) + 1 > PATH_MAX) {
        PRINT_ERR("filename(%s) too long.\n", filename);
        return -1;
    }
    ret = strncat_s(path, PATH_MAX, filename, strlen(filename));
    if (ret != 0) {
        PRINT_ERR("strncat_s failed, (%s, %d, %s, %d), errno = %d\n",
                  path, PATH_MAX, filename, (int32_t)strlen(filename) + 1, errno);
        return -1;
    }

    fp = fopen(path, "wb");
    if (fp == NULL) {
        PRINT_ERR("can`t create file(%s), errno = %d\n", path, errno);
        return -1;
    }

    size_t retFwrite = fwrite(convertedBuffer, bufferLen, 1, fp);
    if (retFwrite != 1) {
        PRINT_ERR("can`t write file(%s),errno = %d\n", path, errno);
        (void)fclose(fp);
        return -1;
    }

    (void)fclose(fp);

    return 0;
}

static cJSON *CreateWholeSyscapJsonObj(void)
{
    size_t numOfSyscapAll = sizeof(arraySyscap) / sizeof(SyscapWithNum);
    cJSON *root =  cJSON_CreateObject();
    for (size_t i = 0; i < numOfSyscapAll; i++) {
        cJSON_AddItemToObject(root, arraySyscap[i].syscapStr, cJSON_CreateNumber(arraySyscap[i].num));
    }
    return root;
}

int32_t RPCIDEncode(char *inputFile, char *outDirPath)
{
    int32_t ret;
    char *contextBuffer = NULL;
    uint32_t bufferLen, sysCapSize;
    char *convertedBuffer = NULL;
    uint32_t convertedBufLen = sizeof(RPCIDHead);
    RPCIDHead *headPtr = NULL;
    char *fillTmpPtr = NULL;
    cJSON *cjsonObjectRoot = NULL;
    cJSON *apiVerItem = NULL;
    cJSON *sysCapPtr = NULL;
    cJSON *arrayItemPtr = NULL;

    ret = GetFileContext(inputFile, &contextBuffer, &bufferLen);
    if (ret != 0) {
        PRINT_ERR("GetFileContext failed, input file : %s\n", inputFile);
        return ret;
    }

    cjsonObjectRoot = cJSON_ParseWithLength(contextBuffer, bufferLen);
    if (cjsonObjectRoot == NULL) {
        PRINT_ERR("cJSON_Parse failed, context buffer is:\n%s\n", contextBuffer);
        ret = -1;
        goto FREE_CONTEXT_OUT;
    }

    sysCapPtr = cJSON_GetObjectItem(cjsonObjectRoot, "syscap");
    if (sysCapPtr == NULL || !cJSON_IsArray(sysCapPtr)) {
        PRINT_ERR("get \"syscap\" object failed.\n");
        ret = -1;
        goto FREE_CONTEXT_OUT;
    }

    ret = cJSON_GetArraySize(sysCapPtr);
    if (ret < 0) {
        PRINT_ERR("get \"syscap\" array size failed\n");
        ret = -1;
        goto FREE_CONTEXT_OUT;
    }
    sysCapSize = (uint32_t)ret;
    // 2, to save SysCaptype & SysCapLength
    convertedBufLen += (2 * sizeof(uint16_t) + sysCapSize * SINGLE_FEAT_LENGTH);

    convertedBuffer = (char *)malloc(convertedBufLen);
    if (convertedBuffer == NULL) {
        PRINT_ERR("malloc failed\n");
        ret = -1;
        goto FREE_CONTEXT_OUT;
    }
    (void)memset_s(convertedBuffer, convertedBufLen, 0, convertedBufLen);

    headPtr = (RPCIDHead *)convertedBuffer;
    apiVerItem = cJSON_GetObjectItem(cjsonObjectRoot, "api_version");
    if (apiVerItem == NULL || !cJSON_IsNumber(apiVerItem)) {
        PRINT_ERR("get \"api_version\" failed\n");
        ret = -1;
        goto FREE_CONVERT_OUT;
    }
    headPtr->apiVersion = HtonsInter((uint16_t)apiVerItem->valueint);
    headPtr->apiVersionType = 1;

    fillTmpPtr = convertedBuffer + sizeof(RPCIDHead);

    *(uint16_t *)fillTmpPtr = HtonsInter(2); // 2, SysCap Type, 2: request Cap
    fillTmpPtr += sizeof(uint16_t);
    // fill osCap Length
    *(uint16_t *)fillTmpPtr = HtonsInter((uint16_t)(sysCapSize * SINGLE_FEAT_LENGTH));
    fillTmpPtr += sizeof(uint16_t);
    for (uint32_t i = 0; i < sysCapSize; i++) {
        arrayItemPtr = cJSON_GetArrayItem(sysCapPtr, i);
        char *pointPos = strchr(arrayItemPtr->valuestring, '.');
        if (pointPos == NULL) {
            PRINT_ERR("context of \"syscap\" array is invalid\n");
            ret = -1;
            goto FREE_CONVERT_OUT;
        }
        ret = strncmp(arrayItemPtr->valuestring, "SystemCapability.", pointPos - arrayItemPtr->valuestring + 1);
        if (ret != 0) {
            PRINT_ERR("context of \"syscap\" array is invalid\n");
            ret = -1;
            goto FREE_CONVERT_OUT;
        }

        ret = memcpy_s(fillTmpPtr, SINGLE_FEAT_LENGTH, pointPos + 1, strlen(pointPos + 1));
        if (ret != 0) {
            PRINT_ERR("context of \"syscap\" array is invalid\n");
            ret = -1;
            goto FREE_CONVERT_OUT;
        }
        fillTmpPtr += SINGLE_FEAT_LENGTH;
    }

    ret = ConvertedContextSaveAsFile(outDirPath, "rpcid.sc", convertedBuffer, convertedBufLen);
    if (ret != 0) {
        PRINT_ERR("ConvertedContextSaveAsFile failed, outDirPath:%s, filename:rpcid.sc\n", outDirPath);
        goto FREE_CONVERT_OUT;
    }

FREE_CONVERT_OUT:
    free(convertedBuffer);
FREE_CONTEXT_OUT:
    FreeContextBuffer(contextBuffer);
    return ret;
}

static int32_t ParseRpcidToJson(char *input, uint32_t inputLen, cJSON *rpcidJson)
{
    uint32_t i;
    int32_t ret = 0;
    uint16_t sysCapLength = NtohsInter(*(uint16_t *)(input + sizeof(uint32_t)));
    uint16_t sysCapCount = sysCapLength / SINGLE_FEAT_LENGTH;
    char *sysCapBegin = input + sizeof(RPCIDHead) + sizeof(uint32_t);
    RPCIDHead *rpcidHeader = (RPCIDHead *)input;
    cJSON *sysCapJson = cJSON_CreateArray();
    for (i = 0; i < sysCapCount; i++) {
        char *temp = sysCapBegin + i * SINGLE_FEAT_LENGTH;
        if (strlen(temp) >= SINGLE_FEAT_LENGTH) {
            PRINT_ERR("Get SysCap failed, string length too long.\n");
            ret = -1;
            goto FREE_SYSCAP_OUT;
        }
        char buffer[SINGLE_FEAT_LENGTH + 17] = "SystemCapability."; // 17, sizeof "SystemCapability."
        
        ret = strncat_s(buffer, sizeof(buffer), temp, SINGLE_FEAT_LENGTH);
        if (ret != EOK) {
            PRINT_ERR("strncat_s failed.\n");
            goto FREE_SYSCAP_OUT;
        }

        if (!cJSON_AddItemToArray(sysCapJson, cJSON_CreateString(buffer))) {
            PRINT_ERR("Add syscap string to json failed.\n");
            ret = -1;
            goto FREE_SYSCAP_OUT;
        }
    }

    if (!cJSON_AddNumberToObject(rpcidJson, "api_version", NtohsInter(rpcidHeader->apiVersion))) {
        PRINT_ERR("Add api_version to json failed.\n");
        ret = -1;
        goto FREE_SYSCAP_OUT;
    }
    if (!cJSON_AddItemToObject(rpcidJson, "syscap", sysCapJson)) {
        PRINT_ERR("Add syscap to json failed.\n");
        ret = -1;
        goto FREE_SYSCAP_OUT;
    }

    return 0;
FREE_SYSCAP_OUT:
    cJSON_Delete(sysCapJson);
    return ret;
}

static int32_t CheckRpcidFormat(char *inputFile, char **Buffer, uint32_t *Len)
{
    uint32_t bufferLen;
    uint16_t sysCaptype, sysCapLength;
    char *contextBuffer = NULL;
    RPCIDHead *rpcidHeader = NULL;

    if (GetFileContext(inputFile, &contextBuffer, &bufferLen)) {
        PRINT_ERR("GetFileContext failed, input file : %s\n", inputFile);
        return -1;
    }
    if (bufferLen < (2 * sizeof(uint32_t))) { // 2, header of rpcid.sc
        PRINT_ERR("Parse file failed(format is invalid), input file : %s\n", inputFile);
        return -1;
    }
    rpcidHeader = (RPCIDHead *)contextBuffer;
    if (rpcidHeader->apiVersionType != 1) {
        PRINT_ERR("Parse file failed(apiVersionType != 1), input file : %s\n", inputFile);
        return -1;
    }
    sysCaptype = NtohsInter(*(uint16_t *)(rpcidHeader + 1));
    if (sysCaptype != 2) { // 2, app syscap type
        PRINT_ERR("Parse file failed(sysCaptype != 2), input file : %s\n", inputFile);
        return -1;
    }
    sysCapLength = NtohsInter(*(uint16_t *)((char *)(rpcidHeader + 1) + sizeof(uint16_t)));
    if (bufferLen < sizeof(RPCIDHead) + sizeof(uint32_t) + sysCapLength) {
        PRINT_ERR("Parse file failed(SysCap length exceeded), input file : %s\n", inputFile);
        return -1;
    }

    *Buffer = contextBuffer;
    *Len = bufferLen;
    return 0;
}

int32_t RPCIDDecode(char *inputFile, char *outDirPath)
{
    int32_t ret = 0;
    char *contextBuffer = NULL;
    char *convertedBuffer = NULL;
    uint32_t bufferLen;

    // check rpcid.sc
    if (CheckRpcidFormat(inputFile, &contextBuffer, &bufferLen)) {
        PRINT_ERR("Check rpcid.sc format failed. Input failed: %s\n", inputFile);
        goto FREE_CONTEXT_OUT;
    }

    // parse rpcid to json
    cJSON *rpcidRoot = cJSON_CreateObject();
    if (ParseRpcidToJson(contextBuffer, bufferLen, rpcidRoot)) {
        PRINT_ERR("Prase rpcid to json failed. Input failed: %s\n", inputFile);
        goto FREE_RPCID_ROOT;
    }

    // save to json file
    convertedBuffer = cJSON_Print(rpcidRoot);
    ret = ConvertedContextSaveAsFile(outDirPath, "rpcid.json", convertedBuffer, strlen(convertedBuffer));
    if (ret != 0) {
        PRINT_ERR("ConvertedContextSaveAsFile failed, outDirPath:%s, filename:rpcid.json\n", outDirPath);
        goto FREE_RPCID_ROOT;
    }

FREE_RPCID_ROOT:
    cJSON_Delete(rpcidRoot);
FREE_CONTEXT_OUT:
    FreeContextBuffer(contextBuffer);
    return ret;
}

static int SetOsSysCapBitMap(uint8_t *out, uint16_t outLen, uint16_t *index, uint16_t indexLen)
{
    uint16_t sector, pos;

    if (outLen != BYTES_OF_OS_SYSCAP) {
        PRINT_ERR("Input array error.\n");
        return -1;
    }

    for (uint16_t i = 0; i < indexLen; i++) {
        sector = index[i] / UINT8_BIT;
        pos = index[i] % UINT8_BIT;
        if (sector >= BYTES_OF_OS_SYSCAP) {
            PRINT_ERR("Syscap num(%u) out of range(120).\n", sector);
            return -1;
        }
        out[sector] |=  (1 << pos);
    }
    return 0;
}

int32_t DecodeRpcidToString(char *inputFile, char *outDirPath)
{
    int32_t ret = 0;
    int32_t sysCapArraySize;
    uint32_t bufferLen, i;
    uint16_t indexOs = 0;
    uint16_t indexPri = 0;
    uint16_t *osSysCapIndex;
    char *contextBuffer = NULL;
    char *priSyscapArray = NULL;
    char *priSyscap = NULL;
    cJSON *cJsonTemp = NULL;
    cJSON *rpcidRoot = NULL;
    cJSON *sysCapDefine = NULL;
    cJSON *sysCapArray = NULL;

    // check rpcid.sc
    if (CheckRpcidFormat(inputFile, &contextBuffer, &bufferLen)) {
        PRINT_ERR("Check rpcid.sc format failed. Input file: %s\n", inputFile);
        goto FREE_CONTEXT_OUT;
    }

    // parse rpcid to json
    rpcidRoot = cJSON_CreateObject();
    if (ParseRpcidToJson(contextBuffer, bufferLen, rpcidRoot)) {
        PRINT_ERR("Prase rpcid to json failed. Input file: %s\n", inputFile);
        goto FREE_RPCID_ROOT;
    }

    // trans to string format
    sysCapDefine =  CreateWholeSyscapJsonObj();
    sysCapArray = cJSON_GetObjectItem(rpcidRoot, "syscap");
    if (sysCapArray == NULL || !cJSON_IsArray(sysCapArray)) {
        PRINT_ERR("Get syscap failed. Input file: %s\n", inputFile);
        goto FREE_WHOLE_SYSCAP;
    }
    sysCapArraySize = cJSON_GetArraySize(sysCapArray);
    if (sysCapArraySize < 0) {
        PRINT_ERR("Get syscap size failed. Input file: %s\n", inputFile);
        goto FREE_WHOLE_SYSCAP;
    }
    // malloc for save os syscap index
    osSysCapIndex = (uint16_t *)malloc(sizeof(uint16_t) * sysCapArraySize);
    if (osSysCapIndex == NULL) {
        PRINT_ERR("malloc failed.\n");
        goto FREE_WHOLE_SYSCAP;
    }
    (void)memset_s(osSysCapIndex, sizeof(uint16_t) * sysCapArraySize,
                   0, sizeof(uint16_t) * sysCapArraySize);
    // malloc for save private syscap string
    priSyscapArray = (char *)malloc(sysCapArraySize * SINGLE_FEAT_LENGTH);
    if (priSyscapArray == NULL) {
        PRINT_ERR("malloc(%u) failed.\n", sysCapArraySize * SINGLE_FEAT_LENGTH);
        goto FREE_MALLOC_OSSYSCAP;
    }
    (void)memset_s(priSyscapArray, sysCapArraySize * SINGLE_FEAT_LENGTH,
                   0, sysCapArraySize * SINGLE_FEAT_LENGTH);
    priSyscap = priSyscapArray;
    // part os syscap and ptivate syscap
    for (i = 0; i < (uint32_t)sysCapArraySize; i++) {
        cJSON *cJsonItem = cJSON_GetArrayItem(sysCapArray, i);
        cJsonTemp = cJSON_GetObjectItem(sysCapDefine, cJsonItem->valuestring);
        if (cJsonTemp != NULL) {
            osSysCapIndex[indexOs++] = cJsonTemp->valueint;
        } else {
            ret = strncpy_s(priSyscapArray, sysCapArraySize * SINGLE_FEAT_LENGTH,
                            cJsonItem->valuestring, SINGLE_FEAT_LENGTH - 1);
            if (ret != EOK) {
                PRINT_ERR("strcpy_s failed.\n");
                goto FREE_MALLOC_PRISYSCAP;
            }
            priSyscapArray += SINGLE_FEAT_LENGTH;
            indexPri++;
        }
    }
    uint32_t outUint[RPCID_OUT_BUFFER] = {0};
    outUint[0] = NtohsInter(((RPCIDHead *)contextBuffer)->apiVersion);
    outUint[1] = NtohsInter(*(uint16_t *)(contextBuffer + sizeof(uint32_t)));
    uint8_t *osOutUint = (uint8_t *)(outUint + 2);
    if (SetOsSysCapBitMap(osOutUint, 120, osSysCapIndex, indexOs)) {  // 120, len of osOutUint
        PRINT_ERR("Set os syscap bit map failed.\n");
        goto FREE_MALLOC_PRISYSCAP;
    }

    uint16_t outBufferLen = U32_TO_STR_MAX_LEN * RPCID_OUT_BUFFER
                            + (SINGLE_FEAT_LENGTH + 1) * sysCapArraySize;
    char *outBuffer = (char *)malloc(outBufferLen);
    if (outBuffer == NULL) {
        PRINT_ERR("malloc(%u) failed.\n", outBufferLen);
        goto FREE_MALLOC_PRISYSCAP;
    }
    (void)memset_s(outBuffer, outBufferLen, 0, outBufferLen);

    ret = sprintf_s(outBuffer, outBufferLen, "%u", outUint[0]);
    if (ret == -1) {
        PRINT_ERR("sprintf_s failed.\n");
        goto FREE_OUTBUFFER;
    }
    for (i = 1; i < RPCID_OUT_BUFFER; i++) {
        ret = sprintf_s(outBuffer, outBufferLen, "%s,%u", outBuffer, outUint[i]);
        if (ret == -1) {
            PRINT_ERR("sprintf_s failed.\n");
            goto FREE_OUTBUFFER;
        }
    }

    for (i = 0; i < indexPri; i++) {
        ret = sprintf_s(outBuffer, outBufferLen, "%s,%s", outBuffer, priSyscap + i * SINGLE_FEAT_LENGTH);
        if (ret == -1) {
            PRINT_ERR("sprintf_s failed.\n");
            goto FREE_OUTBUFFER;
        }
    }

    const char outputFilename[] = "RPCID.txt";
    ret = ConvertedContextSaveAsFile(outDirPath, outputFilename, outBuffer, strlen(outBuffer));
    if (ret != 0) {
        PRINT_ERR("Save to txt file failed. Output path:%s/%s\n", outDirPath, outputFilename);
        goto FREE_OUTBUFFER;
    }

FREE_OUTBUFFER:
    free(outBuffer);
FREE_MALLOC_PRISYSCAP:
    free(priSyscap);
FREE_MALLOC_OSSYSCAP:
    free(osSysCapIndex);
FREE_WHOLE_SYSCAP:
    cJSON_Delete(sysCapDefine);
FREE_RPCID_ROOT:
    cJSON_Delete(rpcidRoot);
FREE_CONTEXT_OUT:
    FreeContextBuffer(contextBuffer);
    return ret;
}