// refs-fclone.cpp : Defines the entry point for the console application.
//

#include "stdafx.h"
#include "Windows.h"
#include "Shlwapi.h"
#pragma comment(lib, "Shlwapi.lib")

/*main info here
	https://msdn.microsoft.com/library/windows/desktop/mt590821(v=vs.85).aspx
	Change Target platform to 10.0.10586.0 in project > properties > General
	Some copy pasting from this (which does basically the same, the exercise is to understand better, not to copycat) https://github.com/0xbadfca11/reflink/blob/master/reflink.cpp


typedef struct _DUPLICATE_EXTENTS_DATA {
HANDLE        FileHandle;
LARGE_INTEGER SourceFileOffset;
LARGE_INTEGER TargetFileOffset;
LARGE_INTEGER ByteCount;
} DUPLICATE_EXTENTS_DATA, *PDUPLICATE_EXTENTS_DATA;

to clone
BOOL
WINAPI
DeviceIoControl( (HANDLE)       hDevice,          // handle to device
FSCTL_DUPLICATE_EXTENTS_TO_FILE, // dwIoControlCode
(LPVOID)       lpInBuffer,       // input buffer
(DWORD)        nInBufferSize,    // size of input buffer
NULL,                            // lpOutBuffer
0,                               // nOutBufferSize
(LPDWORD)      lpBytesReturned,  // number of bytes returned
(LPOVERLAPPED) lpOverlapped );   // OVERLAPPED structure


*/

#define SUPERMAXPATH 4096
#define ERRORWIDTH 4096
#define CLONESZ 1073741824

//just a generic function to print out the last error in a readable format
void printLastError(LPCWSTR errdetails) {
	wchar_t errorbuffer[ERRORWIDTH];
	FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM, NULL, GetLastError(), MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), errorbuffer, ERRORWIDTH, NULL);
	wprintf(L"%ls : %ls\n", errdetails,errorbuffer);
}

int main(int argc, char* argv[])
{
	//return code != 0 => error
	int returncode = 0;
	//some api call require a pointer to a dword although it is not used
	LPDWORD dummyptr = { 0 };

	//need at least src and dest file
	if (argc > 2) {
		//converting regular char* to wide char because most windows api call accept it
		wchar_t src[SUPERMAXPATH] = { 0 };
		MultiByteToWideChar(0, 0, argv[1], strlen(argv[1]), src, strlen(argv[1]));

		wchar_t tgt[SUPERMAXPATH] = { 0 };
		MultiByteToWideChar(0, 0, argv[2], strlen(argv[2]), tgt, strlen(argv[2]));

		//check if source exits and make sure target does not exists
		if (PathFileExists(src)) {
			if (!PathFileExists(tgt)) {

				//getting the full path (although it is just for visualisation)
				wchar_t fullsrc[SUPERMAXPATH] = { 0 };
				TCHAR** lppsrcPart = { NULL };
				wchar_t fulltgt[SUPERMAXPATH] = { 0 };
				TCHAR** lpptgtPart = { NULL };

				GetFullPathName(src, SUPERMAXPATH, fullsrc, lppsrcPart);
				GetFullPathName(tgt, SUPERMAXPATH, fulltgt, lpptgtPart);

				//opening the source path for reading
				HANDLE srchandle = CreateFile(fullsrc, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);

				//if we can open it query details
				if (srchandle != INVALID_HANDLE_VALUE) {

					//what is the file size
					LARGE_INTEGER filesz = { 0 };
					GetFileSizeEx(srchandle,&filesz);

					//basic file info, required to check if the file is sparse (copied from other project)
					FILE_BASIC_INFO filebasicinfo = { 0 };
					GetFileInformationByHandleEx(srchandle, FileBasicInfo, &filebasicinfo, sizeof(filebasicinfo));
					
					//check if the filesystem allows cloning
					ULONG fsflags = 0;
					GetVolumeInformationByHandleW(srchandle, NULL, 0, NULL, NULL, &fsflags, NULL, 0);
						
					if (fsflags & FILE_SUPPORTS_BLOCK_REFCOUNTING)
					{
						//opening the target file for writing
						//create always
						HANDLE tgthandle = CreateFile(fulltgt, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
						if (tgthandle != INVALID_HANDLE_VALUE) {
							//https://technet.microsoft.com/en-us/windows-server-docs/storage/refs/block-cloning
							//must be sparse or not sparse
							//must have same integrity or not
							//must have same file length
							DWORD preallocerr = 0;
							DWORD written = 0;

							//make sure the file is sparse if the sourse is sparse
							//it makes sense that target is sparse so it doesn't consume disk space for blocks that are 0
							if (preallocerr == 0) {
								if (filebasicinfo.FileAttributes | FILE_ATTRIBUTE_SPARSE_FILE) {
									printf("Original file is sparse, copying\n");
									FILE_SET_SPARSE_BUFFER sparse = { true };
									DeviceIoControl(tgthandle, FSCTL_SET_SPARSE, &sparse, sizeof(sparse), NULL, 0, dummyptr, NULL);
									preallocerr = GetLastError();
								}
							}
							//integrity scan info must be the same for block cloning in both files
							if (preallocerr == 0) {
								FSCTL_GET_INTEGRITY_INFORMATION_BUFFER integinfo = { 0 };

								//query the info from src
								if (DeviceIoControl(srchandle, FSCTL_GET_INTEGRITY_INFORMATION, NULL, 0, &integinfo, sizeof(integinfo), &written, NULL)) {
									printf("Copied integrity info (%d)\n",integinfo.ChecksumAlgorithm);
									//setting the info to tgt
									DeviceIoControl(tgthandle, FSCTL_SET_INTEGRITY_INFORMATION, &integinfo, sizeof(integinfo), NULL, 0, dummyptr, NULL);
									preallocerr = GetLastError();
								}
							}
							//setting the file end of the target to the size of the source
							//this basically makes the file as big as the source instead of 0kb, before cloning
							//sparse setting should be done first so it doesn't consume space on disk
							if (preallocerr == 0) {
								FILE_END_OF_FILE_INFO preallocsz = { filesz };
								printf("Setting file end at %lld\n", filesz);
								SetFileInformationByHandle(tgthandle, FileEndOfFileInfo, &preallocsz, sizeof(preallocsz));
								preallocerr = GetLastError();
							}



							if (preallocerr == 0 && tgthandle != INVALID_HANDLE_VALUE && srchandle != INVALID_HANDLE_VALUE) {
								DWORD cperr = 0;

								//file handle are setup, we can start cloning
								wprintf(L"ReFS CP : \n%ls (%lld)\n -> %ls\n", fullsrc, filesz, fulltgt);


								//longlong required because basic long is only value of +- 4GB
								//also the block clone require large integers which are basically struct with longlong integers for 64bit server
								//the clone also copies a max of 4GB per time, however here it is limited to CLONESZ
								for (LONGLONG cpoffset = 0; cpoffset < filesz.QuadPart && cperr == 0; cpoffset += CLONESZ) {
									LONGLONG cpblocks = CLONESZ;

									//if the offset + the amount of blocks is bigger then CLONESZ, we need to copy a smaller amount
									if ((cpoffset + cpblocks) > filesz.QuadPart) {
										cpblocks = filesz.QuadPart - cpoffset;
									}


									//setting up the struct. since we want identical files, we put the offset the same
									DUPLICATE_EXTENTS_DATA clonestruct = { srchandle };
									clonestruct.FileHandle = srchandle;
									clonestruct.ByteCount.QuadPart = cpblocks;
									clonestruct.SourceFileOffset.QuadPart = cpoffset;
									clonestruct.TargetFileOffset.QuadPart = cpoffset;

									wprintf(L"Cloning offset %lld size %lld\n", clonestruct.SourceFileOffset.QuadPart, clonestruct.ByteCount.QuadPart);

									//calling the duplicate "API" with out previous defined struct
									DeviceIoControl(tgthandle, FSCTL_DUPLICATE_EXTENTS_TO_FILE, &clonestruct, sizeof(clonestruct), NULL, 0, dummyptr, NULL);

									cperr = GetLastError();
								}

								if (cperr) {
									printLastError(L"Error issuing Device IO Control statement (sure this is ReFS3.1 on Windows 2016?)");
									returncode = 20;
								}
								else {
									printf("Cloned without errors\n");
								}

							}
							else {
								printLastError(L"Error preallocating file");
								returncode = 17;
							}

							CloseHandle(tgthandle);
						}
						else {
							printLastError(L"Issue opening target handle");
							returncode = 16;
						}
					}
					else {
						printf("Original file does not support cloning");
						returncode = 15;
					}
					CloseHandle(srchandle);
				}
				else {
					printLastError(L"Issue opening source handle");
					returncode = 14;
				}
			}
			else {
				printf("Tgt already exists %s\n",argv[2]);
				returncode = 13;
			}
		}
		else {
			printf("Src does not exists %s\n",argv[1]);
			returncode = 12;
		}
	}
	else {
		printf("refs-fclone.exe <src> <tgt>\n");
		returncode = 11;
	}
    return returncode;
}

