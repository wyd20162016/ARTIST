//
// Created by Lukas on 8/16/2015.
//


#include <string.h>
#include <sys/types.h>
#include <stdbool.h>
#include "oat.h"
#include "../logging.h"

bool oat_Setup(struct OatFile* result, void *mem_begin, void *mem_end)
{
    CHECK_RETURNFALSE(mem_begin != NULL && mem_end != NULL);
    CHECK_RETURNFALSE(mem_end > mem_begin);

    result->begin = mem_begin;
    result->end = mem_end;
    result->header = (struct OatHeader*) mem_begin;
    result->key_value_storage_start = &result->header->key_value_store_[0];
    result->dex_file_storage_start = result->key_value_storage_start + result->header->key_value_store_size_;
    return true;
}

bool oat_FindDexFile(struct OatFile* oat_file, struct OatDexFile* result, const char* location)
{
    CHECK_RETURNFALSE(oat_file != NULL);
    CHECK_RETURNFALSE(result != NULL);
    CHECK_RETURNFALSE(location != NULL);

    const void* data = GetDexFileStoragePointer(oat_file->header);
    for(uint32_t i = 0; i < NumDexFiles(oat_file->header); i++)
    {
        if(!ReadOatDexFileData(&data, oat_file->end, &result->data, oat_file->header))
        {
            LOGF("Error decoding oat dex file #%d", i);
            // Decoding only fails when running out of bounds. In
            // that case the other ones shouldn't be valid either.
            return false;
        }
        result->oat_file = oat_file;
        result->index = i;

        if(strlen(location) != result->data.location_string.length)
        {
            continue;
        }
        if(strncmp(location, result->data.location_string.content, strlen(location)) == 0)
        {
            return true;
        }
    }
    // Not found
    return false;
}

bool oat_FindClass(const struct OatDexFile* oat_dex_file, struct OatClass * clazz, char* descriptor)
{
    CHECK_RETURNFALSE(oat_dex_file != NULL);
    CHECK_RETURNFALSE(clazz != NULL);
    CHECK_RETURNFALSE(descriptor != NULL);

    if(!dex_FindClass(oat_dex_file->data.dex_file_pointer, &clazz->dex_class, descriptor))
    {
        return false;
    }

    uint32_t class_def_index = GetIndexForClassDef(oat_dex_file->data.dex_file_pointer, clazz->dex_class.class_def);
    void* oat_class_def_pointer = oat_dex_file->oat_file->begin + oat_dex_file->data.class_definition_offsets[class_def_index];
    if(!DecodeOatClassData(oat_class_def_pointer, oat_dex_file->oat_file->end, &clazz->oat_class_data))
    {
        LOGF("Error decoding OatClassData %s at index %d in OatDexFile %s.", descriptor, class_def_index, oat_dex_file->data.location_string.content);
        return false;
    }
    clazz->oat_dex_file = oat_dex_file;
    return true;
}

bool oat_FindDirectMethod(const struct OatClass * oat_class, struct OatMethod* result, const char* descriptor, const char* signature)
{
    CHECK_RETURNFALSE(oat_class != NULL);
    CHECK_RETURNFALSE(result != NULL);
    CHECK_RETURNFALSE(descriptor != NULL);
    CHECK_RETURNFALSE(signature != NULL);

    if(!dex_FindDirectMethod(&oat_class->dex_class, &result->dex_method, descriptor, signature))
    {
        return false;
    }
    /*
     * GetOatMethodOffsets can legitimately return NULL!
     * Do not break out of here by returning false if this call does not succeed.
     * This can be NULL when the method was simply not AOT-compiled.
     * If this is the case the VM interprets the DEX bytecode instead.
     */
    const struct OatMethodOffsets* meth_off = GetOatMethodOffsets(&oat_class->oat_class_data, result->dex_method.class_method_idx);
    result->oat_method_offsets = meth_off;
    result->oat_class = oat_class;
    return true;
}
bool oat_FindVirtualMethod(const struct OatClass * oat_class, struct OatMethod* result, const char* descriptor, const char* signature)
{
    CHECK_RETURNFALSE(oat_class != NULL);
    CHECK_RETURNFALSE(result != NULL);
    CHECK_RETURNFALSE(descriptor != NULL);
    CHECK_RETURNFALSE(signature != NULL);

    if(!dex_FindVirtualMethod(&oat_class->dex_class, &result->dex_method, descriptor, signature))
    {
        return false;
    }
    /*
     * GetOatMethodOffsets can legitimately return NULL!
     * Do not break out of here by returning false if this call does not succeed.
     * This can be NULL when the method was simply not AOT-compiled.
     * If this is the case the VM interprets the DEX bytecode instead.
     */
    const struct OatMethodOffsets* meth_off = GetOatMethodOffsets(&oat_class->oat_class_data, result->dex_method.class_method_idx);
    result->oat_method_offsets = meth_off;
    result->oat_class = oat_class;
    return true;
}

bool oat_HasQuickCompiledCode(const struct OatMethod* m)
{
    CHECK_RETURNFALSE(m != NULL);
    return m->oat_method_offsets != NULL;
}
void* oat_GetQuickCompiledEntryPoint(const struct OatMethod* m)
{
    CHECK_RETURNNULL(m != NULL);
    void* oat_base = m->oat_class->oat_dex_file->oat_file->begin;
    return m->oat_method_offsets->code_offset_ + oat_base;
}
void* oat_GetQuickCompiledMemoryPointer(const struct OatMethod* m)
{
    return (void*)EntryPointToCodePointer(oat_GetQuickCompiledEntryPoint(m));
}














