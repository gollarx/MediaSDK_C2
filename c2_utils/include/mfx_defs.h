/********************************************************************************

INTEL CORPORATION PROPRIETARY INFORMATION
This software is supplied under the terms of a license agreement or nondisclosure
agreement with Intel Corporation and may not be copied or disclosed except in
accordance with the terms of that agreement
Copyright(c) 2017 Intel Corporation. All Rights Reserved.

*********************************************************************************/

#ifndef __MFX_DEFS_H__
#define __MFX_DEFS_H__

#define MFX_MAX_PATH 260

#define MFX_GET_ARRAY_SIZE(_array) \
    sizeof(_array)/sizeof(_array[0])

#define MFX_CLASS_NO_COPY(class_name) \
    class_name(const class_name&) = delete; \
    class_name& operator=(const class_name&) = delete;

#define EXPORT __attribute__((visibility("default")))

#endif // #ifndef __MFX_DEFS_H__