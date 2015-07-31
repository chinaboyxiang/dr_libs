// Version 0.1 - Public Domain. See "unlicense" statement at the end of this file.

#include "easy_vfs_zip.h"
#include "../easy_vfs.h"

#define MINIZ_HEADER_FILE_ONLY
#include "miniz.c"

#include <assert.h>
#include <string.h>

typedef struct
{
    /// The current index of the iterator. When this hits the file count, the iteration is finished.
    unsigned int index;

}easyvfs_iterator_zip;

typedef struct
{
    /// The file index within the archive.
    mz_uint index;

    /// A pointer to the buffer containing the entire uncompressed data of the file. Unfortunately this is the only way I'm aware of for
    /// reading file data from miniz.c so we'll just stick with it for now. We use a pointer to an 8-bit type so we can easily calculate
    /// offsets.
    mz_uint8* pData;

    /// The size of the file in bytes so we can guard against overflowing reads.
    size_t sizeInBytes;

    /// The current position of the file's read pointer.
    size_t readPointer;

}easyvfs_openedfile_zip;



int           easyvfs_isvalidarchive_zip(easyvfs_context* pContext, const char* path);
void*         easyvfs_openarchive_zip   (easyvfs_file* pFile, easyvfs_accessmode accessMode);
void          easyvfs_closearchive_zip  (easyvfs_archive* pArchive);
int           easyvfs_getfileinfo_zip   (easyvfs_archive* pArchive, const char* path, easyvfs_fileinfo *fi);
void*         easyvfs_beginiteration_zip(easyvfs_archive* pArchive, const char* path);
void          easyvfs_enditeration_zip  (easyvfs_archive* pArchive, easyvfs_iterator* i);
int           easyvfs_nextiteration_zip (easyvfs_archive* pArchive, easyvfs_iterator* i, easyvfs_fileinfo* fi);
void*         easyvfs_openfile_zip      (easyvfs_archive* pArchive, const char* path, easyvfs_accessmode accessMode);
void          easyvfs_closefile_zip     (easyvfs_file* pFile);
int           easyvfs_readfile_zip      (easyvfs_file* pFile, void* dst, unsigned int bytesToRead, unsigned int* bytesReadOut);
int           easyvfs_writefile_zip     (easyvfs_file* pFile, const void* src, unsigned int bytesToWrite, unsigned int* bytesWrittenOut);
easyvfs_bool  easyvfs_seekfile_zip      (easyvfs_file* pFile, easyvfs_int64 bytesToSeek, easyvfs_seekorigin origin);
easyvfs_int64 easyvfs_tellfile_zip      (easyvfs_file* pFile);
easyvfs_int64 easyvfs_filesize_zip      (easyvfs_file* pFile);
int           easyvfs_deletefile_myl    (easyvfs_archive* pArchive, const char* path);
int           easyvfs_renamefile_zip    (easyvfs_archive* pArchive, const char* pathOld, const char* pathNew);
int           easyvfs_mkdir_zip         (easyvfs_archive* pArchive, const char* path);

void easyvfs_registerarchivecallbacks_zip(easyvfs_context* pContext)
{
    easyvfs_archive_callbacks callbacks;
    callbacks.isvalidarchive = easyvfs_isvalidarchive_zip;
    callbacks.openarchive    = easyvfs_openarchive_zip;
    callbacks.closearchive   = easyvfs_closearchive_zip;
    callbacks.getfileinfo    = easyvfs_getfileinfo_zip;
    callbacks.beginiteration = easyvfs_beginiteration_zip;
    callbacks.enditeration   = easyvfs_enditeration_zip;
    callbacks.nextiteration  = easyvfs_nextiteration_zip;
    callbacks.openfile       = easyvfs_openfile_zip;
    callbacks.closefile      = easyvfs_closefile_zip;
    callbacks.readfile       = easyvfs_readfile_zip;
    callbacks.writefile      = easyvfs_writefile_zip;
    callbacks.seekfile       = easyvfs_seekfile_zip;
    callbacks.tellfile       = easyvfs_tellfile_zip;
    callbacks.filesize       = easyvfs_filesize_zip;
    callbacks.deletefile     = easyvfs_deletefile_myl;
    callbacks.renamefile     = easyvfs_renamefile_zip;
    callbacks.mkdir          = easyvfs_mkdir_zip;
    easyvfs_registerarchivecallbacks(pContext, callbacks);
}


int easyvfs_isvalidarchive_zip(easyvfs_context* pContext, const char* path)
{
    (void)pContext;

    if (easyvfs_extensionequal(path, "zip"))
    {
        return 1;
    }

    return 0;
}


size_t easyvfs_mz_file_read_func(void *pOpaque, mz_uint64 file_ofs, void *pBuf, size_t n)
{
    // The opaque type is a pointer to a easyvfs_file object which represents the file of the archive.
    easyvfs_file* pZipFile = pOpaque;
    assert(pZipFile != NULL);

    easyvfs_seekfile(pZipFile, (easyvfs_int64)file_ofs, easyvfs_start);

    unsigned int bytesRead;
    int result = easyvfs_readfile(pZipFile, pBuf, n, &bytesRead);
    if (result == 0)
    {
        // Failed to read the file.
        bytesRead = 0;
    }

    return bytesRead;
}


void* easyvfs_openarchive_zip(easyvfs_file* pFile, easyvfs_accessmode accessMode)
{
    assert(pFile != NULL);
    assert(easyvfs_tellfile(pFile) == 0);

    // Only support read-only mode at the moment.
    if (accessMode == easyvfs_write || accessMode == easyvfs_readwrite)
    {
        return NULL;
    }


    mz_zip_archive* pZip = easyvfs_malloc(sizeof(mz_zip_archive));
    if (pZip != NULL)
    {
        pZip->m_pRead        = easyvfs_mz_file_read_func;
        pZip->m_pIO_opaque   = pFile;
        if (mz_zip_reader_init(pZip, pZip->m_archive_size, 0))
        {
            // Everything is good so far...
        }
        else
        {
            easyvfs_free(pZip);
            pZip = NULL;
        }
    }

    return pZip;
}

void easyvfs_closearchive_zip(easyvfs_archive* pArchive)
{
    assert(pArchive != NULL);
    assert(pArchive->pUserData != NULL);

    mz_zip_reader_end(pArchive->pUserData);
    easyvfs_free(pArchive->pUserData);
}

int easyvfs_getfileinfo_zip(easyvfs_archive* pArchive, const char* path, easyvfs_fileinfo *fi)
{
    assert(pArchive != NULL);
    assert(pArchive->pUserData != NULL);

    mz_zip_archive* pZip = pArchive->pUserData;
    int fileIndex = mz_zip_reader_locate_file(pZip, path, NULL, MZ_ZIP_FLAG_CASE_SENSITIVE);
    if (fileIndex != -1)
    {
        if (fi != NULL)
        {
            mz_zip_archive_file_stat zipStat;
            if (mz_zip_reader_file_stat(pZip, fileIndex, &zipStat))
            {
                easyvfs_copyandappendpath(fi->absolutePath, EASYVFS_MAX_PATH, pArchive->absolutePath, path);
                fi->sizeInBytes      = (easyvfs_int64)zipStat.m_uncomp_size;
                fi->lastModifiedTime = zipStat.m_time;
                fi->attributes       = EASYVFS_FILE_ATTRIBUTE_READONLY;
                if (mz_zip_reader_is_file_a_directory(pZip, fileIndex))
                {
                    fi->attributes |= EASYVFS_FILE_ATTRIBUTE_DIRECTORY;
                }

                return 1;
            }
        }
        
        return 1;
    }
    else
    {
        // There was an error finding the file. It probably doesn't exist.
    }

    return 0;
}

void* easyvfs_beginiteration_zip(easyvfs_archive* pArchive, const char* path)
{
    assert(pArchive != 0);
    assert(path != NULL);

    return NULL;
}

void easyvfs_enditeration_zip(easyvfs_archive* pArchive, easyvfs_iterator* i)
{
    assert(pArchive != 0);
    assert(i != NULL);

    easyvfs_free(i->pUserData);
}

int easyvfs_nextiteration_zip(easyvfs_archive* pArchive, easyvfs_iterator* i, easyvfs_fileinfo* fi)
{
    assert(pArchive != 0);
    assert(i != NULL);

    (void)fi;

    return 0;
}

void* easyvfs_openfile_zip(easyvfs_archive* pArchive, const char* path, easyvfs_accessmode accessMode)
{
    assert(pArchive != 0);
    assert(pArchive->pUserData != NULL);
    assert(path != NULL);

    // Only supporting read-only for now.
    if (accessMode == easyvfs_write || accessMode == easyvfs_readwrite)
    {
        return NULL;
    }


    mz_zip_archive* pZip = pArchive->pUserData;
    int fileIndex = mz_zip_reader_locate_file(pZip, path, NULL, MZ_ZIP_FLAG_CASE_SENSITIVE);
    if (fileIndex != -1)
    {
        easyvfs_openedfile_zip* pOpenedFile = easyvfs_malloc(sizeof(easyvfs_openedfile_zip));
        if (pOpenedFile != NULL)
        {
            pOpenedFile->pData = mz_zip_reader_extract_to_heap(pZip, fileIndex, &pOpenedFile->sizeInBytes, 0);
            if (pOpenedFile->pData != NULL)
            {
                pOpenedFile->index       = (mz_uint)fileIndex;
                pOpenedFile->readPointer = 0;
            }
            else
            {
                // Failed to read the data.
                easyvfs_free(pOpenedFile);
                pOpenedFile = NULL;
            }
        }

        return pOpenedFile;
    }
    else
    {
        // Couldn't find the file.
    }

    return NULL;
}

void easyvfs_closefile_zip(easyvfs_file* pFile)
{
    assert(pFile != 0);

    easyvfs_openedfile_zip* pOpenedFile = pFile->pUserData;
    if (pOpenedFile != NULL)
    {
        mz_zip_archive* pZip = pFile->pArchive->pUserData;
        assert(pZip != NULL);

        pZip->m_pFree(pZip->m_pAlloc_opaque, pOpenedFile->pData);
        easyvfs_free(pOpenedFile);
    }
}

int easyvfs_readfile_zip(easyvfs_file* pFile, void* dst, unsigned int bytesToRead, unsigned int* bytesReadOut)
{
    assert(pFile != 0);
    assert(dst != NULL);
    assert(bytesToRead > 0);

    easyvfs_openedfile_zip* pOpenedFile = pFile->pUserData;
    if (pOpenedFile != NULL)
    {
        (void)bytesReadOut;
    }

    return 0;
}

int easyvfs_writefile_zip(easyvfs_file* pFile, const void* src, unsigned int bytesToWrite, unsigned int* bytesWrittenOut)
{
    assert(pFile != 0);
    assert(src != NULL);
    assert(bytesToWrite > 0);

    // All files are read-only for now.
    if (bytesWrittenOut != NULL)
    {
        *bytesWrittenOut = 0;
    }

    return 0;
}

easyvfs_bool easyvfs_seekfile_zip(easyvfs_file* pFile, easyvfs_int64 bytesToSeek, easyvfs_seekorigin origin)
{
    assert(pFile != 0);

    easyvfs_openedfile_zip* pOpenedFile = pFile->pUserData;
    if (pOpenedFile != NULL)
    {
        (void)origin;
        (void)bytesToSeek;
    }

    return 0;
}

easyvfs_int64 easyvfs_tellfile_zip(easyvfs_file* pFile)
{
    assert(pFile != 0);

    easyvfs_openedfile_zip* pOpenedFile = pFile->pUserData;
    if (pOpenedFile != NULL)
    {
    }

    return 0;
}

easyvfs_int64 easyvfs_filesize_zip(easyvfs_file* pFile)
{
    assert(pFile != 0);

    easyvfs_openedfile_zip* pOpenedFile = pFile->pUserData;
    if (pOpenedFile != NULL)
    {
    }

    return 0;
}

int easyvfs_deletefile_zip(easyvfs_archive* pArchive, const char* path)
{
    assert(pArchive != 0);
    assert(path     != 0);

    // All files are read-only for now.
    return 0;
}

int easyvfs_renamefile_zip(easyvfs_archive* pArchive, const char* pathOld, const char* pathNew)
{
    assert(pArchive != 0);
    assert(pathOld  != 0);
    assert(pathNew  != 0);

    // All files are read-only for now.
    return 0;
}

int easyvfs_mkdir_zip(easyvfs_archive* pArchive, const char* path)
{
    assert(pArchive != 0);
    assert(path     != 0);

    // All files are read-only for now.
    return 0;
}




/*
This is free and unencumbered software released into the public domain.

Anyone is free to copy, modify, publish, use, compile, sell, or
distribute this software, either in source code form or as a compiled
binary, for any purpose, commercial or non-commercial, and by any
means.

In jurisdictions that recognize copyright laws, the author or authors
of this software dedicate any and all copyright interest in the
software to the public domain. We make this dedication for the benefit
of the public at large and to the detriment of our heirs and
successors. We intend this dedication to be an overt act of
relinquishment in perpetuity of all present and future rights to this
software under copyright law.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
OTHER DEALINGS IN THE SOFTWARE.

For more information, please refer to <http://unlicense.org/>
*/