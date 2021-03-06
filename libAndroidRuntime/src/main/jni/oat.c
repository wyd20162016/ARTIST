/*
 * Copyright 2016 Lukas Dresel
 *
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
 
//
// Created by Lukas on 8/16/2015.
//

#ifdef __cplusplus
extern "C" {
#endif

#include <string.h>
#include <sys/types.h>
#include <stdbool.h>

#include <hostsystem/abi.h>
#include "oat.h"
#include <utility/logging.h>


void *oat_PointerFromFileOffset(const struct OatFile *oat_file, uint32_t offset) {
    CHECK_RETURNNULL(oat_file != NULL);
    if (offset == 0) {
        // If an offset is set to zero, it is invalid.
        // This would not make any sense as this would have to point to the OatFile Header.
        return NULL;
    }
    return oat_file->begin + offset;
}

bool oat_IsValidHeader(void *mem)
{
    return IsValidOatHeader(mem);
}

bool oat_Setup(struct OatFile *result, void *mem_begin, void *mem_end) {
    CHECK_RETURNFALSE(mem_begin != NULL && mem_end != NULL);
    CHECK_RETURNFALSE(mem_end > mem_begin);

    result->begin = mem_begin;
    result->end = mem_end;
    result->header = (struct OatHeader *) mem_begin;
    result->key_value_storage_start = &result->header->key_value_store_[0];
    result->dex_file_storage_start = result->key_value_storage_start + result->header->key_value_store_size_;
    return true;
}

bool oat_FindDexFile(struct OatFile *oat_file, struct OatDexFile *result, const char *location) {
    CHECK_RETURNFALSE(oat_file != NULL);
    CHECK_RETURNFALSE(result != NULL);
    CHECK_RETURNFALSE(location != NULL);

    const void *data = GetDexFileStoragePointer(oat_file->header);
    for (uint32_t i = 0; i < NumDexFiles(oat_file->header); i++) {
        if (!ReadOatDexFileData(&data, oat_file->end, &result->data, oat_file->header)) {
            LOGF("Error decoding oat dex file #%d", i);
            // Decoding only fails when running out of bounds. In
            // that case the other ones shouldn't be valid either.
            return false;
        }
        result->oat_file = oat_file;
        result->index = i;

        if (strlen(location) != result->data.location_string.length) {
            continue;
        }
        if (strncmp(location, result->data.location_string.content, strlen(location)) == 0) {
            return true;
        }
    }
    // Not found
    return false;
}

bool oat_GetOatDexFile(struct OatFile *oat_file, struct OatDexFile *result, uint32_t index)
{
    CHECK_RETURNFALSE(oat_file != NULL);
    CHECK_RETURNFALSE(result != NULL);
    CHECK_RETURNFALSE(index < NumDexFiles(oat_file->header));

    const void *data = GetDexFileStoragePointer(oat_file->header);
    for (uint32_t i = 0; i < index + 1; i++) {
        // just continue reading through the OatDexFiles until we reach our desired index
        if (!ReadOatDexFileData(&data, oat_file->end, &result->data, oat_file->header)) {
            LOGF("Error decoding oat dex file #%d", i);
            // Decoding only fails when running out of bounds. In
            // that case the other ones shouldn't be valid either.
            return false;
        }
    }
    result->oat_file = oat_file;
    result->index = index;
    return true;

}

bool oat_FindClass(struct OatFile* oat, struct OatDexFile* result_oat_dex_file,
                   struct OatClass *result_clazz, char *descriptor)
{
    struct OatDexFile oat_dex_ignored;
    struct OatClass oat_class_ignored;
    CHECK_RETURNFALSE(oat != NULL);
    CHECK_RETURNFALSE(descriptor != NULL);
    if (result_oat_dex_file == NULL)
    {
        result_oat_dex_file = &oat_dex_ignored;
    }
    if (result_clazz == NULL)
    {
        result_clazz = &oat_class_ignored;
    }
    for(uint32_t i = 0; i < oat->header->dex_file_count_; i++)
    {
        if(!oat_GetOatDexFile(oat, result_oat_dex_file, i))
        {
            return false;
        }
        if(oat_FindClassInDex(result_oat_dex_file, result_clazz, descriptor))
        {
            return true;
        }
    }
    // Could not find the OatDexFile, so we return false
    return false;
}
bool oat_FindClassInDex(const struct OatDexFile *oat_dex_file, struct OatClass *clazz,
                    const char *descriptor)
{
    CHECK_RETURNFALSE(oat_dex_file != NULL);
    CHECK_RETURNFALSE(clazz != NULL);
    CHECK_RETURNFALSE(descriptor != NULL);

    if (!dex_FindClass(oat_dex_file->data.dex_file_pointer, &clazz->dex_class, descriptor)) {
        return false;
    }

    uint32_t class_def_index = GetIndexForClassDef(oat_dex_file->data.dex_file_pointer,
                                                   clazz->dex_class.class_def);
    uint32_t class_def_offset = oat_dex_file->data.class_definition_offsets[class_def_index];

    void *oat_class_def_pointer = oat_PointerFromFileOffset(oat_dex_file->oat_file,
                                                            class_def_offset);
    if (!DecodeOatClassData(oat_class_def_pointer, oat_dex_file->oat_file->end,
                            &clazz->oat_class_data)) {
        LOGF("Error decoding OatClassData %s at index %d in OatDexFile %s.", descriptor,
             class_def_index, oat_dex_file->data.location_string.content);
        return false;
    }
    clazz->oat_dex_file = oat_dex_file;
    return true;
}

bool oat_GetClass(const struct OatDexFile *oat_dex_file, struct OatClass *clazz,
                  uint16_t class_def_index) {
    CHECK_RETURNFALSE(oat_dex_file != NULL);
    CHECK_RETURNFALSE(clazz != NULL);

    // Unnecessary check as the dex_GetClass call should check this. If it doesn't that is horrible.
    // CHECK(class_def_index < dex_NumberOfClassDefs(oat_dex_file->data.dex_file_pointer));

    if (!dex_GetClass(oat_dex_file->data.dex_file_pointer, &clazz->dex_class, class_def_index)) {
        return false;
    }

    uint32_t class_def_offset = oat_dex_file->data.class_definition_offsets[class_def_index];

    void *oat_class_def_pointer = oat_PointerFromFileOffset(oat_dex_file->oat_file,
                                                            class_def_offset);
    if (!DecodeOatClassData(oat_class_def_pointer, oat_dex_file->oat_file->end,
                            &clazz->oat_class_data)) {
        LOGF("Error decoding OatClassData at index %d in OatDexFile %s.", class_def_index,
             oat_dex_file->data.location_string.content);
        return false;
    }
    clazz->oat_dex_file = oat_dex_file;
    return true;
}

bool oat_FindDirectMethod(const struct OatClass *oat_class, struct OatMethod *result,
                          const char *descriptor, const char *signature) {
    CHECK_RETURNFALSE(oat_class != NULL);
    CHECK_RETURNFALSE(result != NULL);
    CHECK_RETURNFALSE(descriptor != NULL);
    CHECK_RETURNFALSE(signature != NULL);

    LOGD("Looking up direct oat method %s %s", descriptor, signature);
    if (!dex_FindDirectMethod(&oat_class->dex_class, &result->dex_method, descriptor, signature))
    {
        LOGD("Could not find direct oat method %s %s", descriptor, signature);
        return false;
    }
    /*
     * GetOatMethodOffsets can legitimately return NULL!
     * Do not break out of here by returning false if this call does not succeed.
     * This can be NULL when the method was simply not AOT-compiled.
     * If this is the case the VM interprets the DEX bytecode instead.
     */

    LOGD("Getting oat method offsets for %s %s", descriptor, signature);
    const struct OatMethodOffsets *meth_off = GetOatMethodOffsets(&oat_class->oat_class_data,
                                                                  result->dex_method.class_method_idx);
    result->oat_method_offsets = meth_off;
    result->oat_class = oat_class;
    return true;
}

bool oat_FindVirtualMethod(const struct OatClass *oat_class, struct OatMethod *result,
                           const char *descriptor, const char *signature)
{
    CHECK_RETURNFALSE(oat_class != NULL);
    CHECK_RETURNFALSE(result != NULL);
    CHECK_RETURNFALSE(descriptor != NULL);
    CHECK_RETURNFALSE(signature != NULL);

    LOGD("Looking up virtual oat method %s %s", descriptor, signature);
    if (!dex_FindVirtualMethod(&oat_class->dex_class, &result->dex_method, descriptor, signature)) {
        return false;
    }
    /*
     * GetOatMethodOffsets can legitimately return NULL!
     * Do not break out of here by returning false if this call does not succeed.
     * This can be NULL when the method was simply not AOT-compiled.
     * If this is the case the VM interprets the DEX bytecode instead.
     */
    LOGD("Getting oat method offsets for %s %s", descriptor, signature);
    const struct OatMethodOffsets *meth_off = GetOatMethodOffsets(&oat_class->oat_class_data,
                                                                  result->dex_method.class_method_idx);
    result->oat_method_offsets = meth_off;
    result->oat_class = oat_class;
    return true;
}
bool oat_FindMethod(const struct OatClass* oat_class, struct OatMethod* result,
                    const char* descriptor, const char* signature)
{
    LOGD("Looking up direct method %s %s", descriptor, signature);
    if(oat_FindDirectMethod(oat_class, result, descriptor, signature))
    {
        return true;
    }
    LOGD("Looking up virtual method %s %s", descriptor, signature);
    if(oat_FindVirtualMethod(oat_class, result, descriptor, signature))
    {
        return true;
    }
    LOGD("Could not find method %s %s", descriptor, signature);
    return false;
}

bool oat_HasQuickCompiledCode(const struct OatMethod *m) {
    CHECK_RETURNFALSE(m != NULL);
    return m->oat_method_offsets != NULL;
}

void *oat_GetQuickCompiledEntryPoint(const struct OatMethod *m) {
    CHECK_RETURNNULL(m != NULL);
    void *oat_base = m->oat_class->oat_dex_file->oat_file->begin;
    return m->oat_method_offsets->code_offset_ + oat_base;
}

void *oat_GetQuickCompiledMemoryPointer(const struct OatMethod *m) {
    return (void *) InstructionPointerToCodePointer(oat_GetQuickCompiledEntryPoint(m));
}


#ifdef __cplusplus
} // extern "C"
#endif










