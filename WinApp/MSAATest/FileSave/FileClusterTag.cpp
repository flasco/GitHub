#include "stdafx.h"
#include "FileClusterTag.h"
#include <time.h>
#include <atlfile.h>

#pragma pack(push, 1)
typedef struct _SAFEBK_BLOCK_HDR
{
	GUID _head;
	uint16_t version;
	uint32_t clustersize;
	GUID fileid;
	uint64_t curr;
	uint64_t total;
	uint16_t size;
	uint64_t crc;
	char filetype[16];
	GUID _tail;

	uint8_t buff[1];

} SAFEBK_BLOCK_HDR, *PSAFEBK_BLOCK_HDR;
#define SAFEBK_HDR_SIZE (SIZE_T)(sizeof(SAFEBK_BLOCK_HDR) - sizeof(((PSAFEBK_BLOCK_HDR)0)->buff))

#define SAFEBK_VERSION_001	1

static const GUID GUID_SAFEBK_ID = { 0x1a90e7c9, 0x4b3a, 0x49a8, { 0xae, 0xa0, 0xe1, 0xd4, 0x8a, 0x25, 0x66, 0xab } };


typedef struct _SAFEBK_FIRST_BLOCK
{
	time_t bktime;
	uint64_t filesize;
	uint16_t filenamelen;
	WCHAR filename[1];
} SAFEBK_FIRST_BLOCK, *PSAFEBK_FIRST_BLOCK;
#pragma pack(pop)

uint64_t CalcBlockCRC(PSAFEBK_BLOCK_HDR pBlock, int nBlock)
{
	uint64_t tmp = pBlock->crc;

	uint64_t crc = 0;
	pBlock->crc = 0;
	for (int i = 0; i < nBlock; i++)
		crc += (crc << 8) + ((uint8_t*)pBlock)[i];

	pBlock->crc = tmp;
	return crc;
}

uint32_t GetBytesPerCluster(LPCWSTR wsFilePath)
{
	DWORD dwSectorPerCluster, dwBytesPerSector, dwNumOfFreeCluster, dwTotalCluster;
	WCHAR szRoot[4] = { L"c:\\" };
	szRoot[0] = *wsFilePath;
	GetDiskFreeSpace(szRoot, &dwSectorPerCluster, &dwBytesPerSector, &dwNumOfFreeCluster, &dwTotalCluster);

	uint32_t clustersize = (uint32_t)dwSectorPerCluster * (uint32_t)dwBytesPerSector;
	return clustersize;
}

void CFileClusterTag::GetFileExtension(LPCWSTR wsFileNoTag, char* wsFileType, int nFileTypeBufferSize)
{
	CAtlString strFileNoTag = wsFileNoTag;
	int nPos = strFileNoTag.ReverseFind(L'.');
	if (nPos != -1)
	{
		CAtlStringA strFileType = CW2A(strFileNoTag.Mid(nPos), CP_UTF8);
		if (!strFileType.IsEmpty())
		{
			memcpy(wsFileType, strFileType, strFileType.GetLength() > nFileTypeBufferSize ? nFileTypeBufferSize : strFileType.GetLength());
		}
	}
}


CFileClusterTag::CFileClusterTag()
{
}


CFileClusterTag::~CFileClusterTag()
{
}

HRESULT CFileClusterTag::AddTag(__in LPCWSTR wsFileNoTag, __in LPCWSTR wsFileTag, __in BOOL bDelOld/* = TRUE*/)
{
	HRESULT hr = E_FAIL;

	do
	{
		int nBlockSize = GetBytesPerCluster(wsFileNoTag);
		int nBuffSize = nBlockSize - SAFEBK_HDR_SIZE;
		char *bfBlock = new char[nBlockSize];	CAutoPtr<char> _auto_free1(bfBlock);
		memset(bfBlock, 0, nBlockSize);

		CAtlFile fSrc;
		hr = fSrc.Create(wsFileNoTag, GENERIC_READ, 0, OPEN_EXISTING);
		if (FAILED(hr))
			break;

		CAtlFile fDst;
		hr = fDst.Create(wsFileTag, GENERIC_WRITE, 0, CREATE_ALWAYS);
		if (FAILED(hr))
			break;

		ULONGLONG ullLen;
		hr = fSrc.GetSize(ullLen);
		if (FAILED(hr))
			break;

		PSAFEBK_BLOCK_HDR pBlock = (PSAFEBK_BLOCK_HDR)bfBlock;
		pBlock->version = SAFEBK_VERSION_001;
		hr = CoCreateGuid(&pBlock->fileid);
		if (FAILED(hr))
			break;
		memcpy(&pBlock->_head, &GUID_SAFEBK_ID, sizeof(GUID_SAFEBK_ID));
		memcpy(&pBlock->_tail, &GUID_SAFEBK_ID, sizeof(GUID_SAFEBK_ID));
		pBlock->total = 1 + (ullLen / nBuffSize) + (ullLen % nBuffSize ? 1 : 0);
		pBlock->curr = 0;
		pBlock->clustersize = nBlockSize;
		GetFileExtension(wsFileNoTag, pBlock->filetype, sizeof(pBlock->filetype));
		

		PSAFEBK_FIRST_BLOCK pInfo = (PSAFEBK_FIRST_BLOCK)pBlock->buff;
		time(&pInfo->bktime);
		pInfo->filesize = (uint64_t)ullLen;
		size_t uNameLen = wcslen(wsFileNoTag);
		pInfo->filenamelen = uNameLen * 2;
		memcpy(pInfo->filename, wsFileNoTag, pInfo->filenamelen);
		pBlock->crc = CalcBlockCRC(pBlock, nBlockSize);

		hr = fDst.Write(pBlock, nBlockSize);
		if (FAILED(hr))
			break;

		while (1)
		{
			pBlock->curr++;
			if (pBlock->curr == pBlock->total - 1)
				memset(pBlock->buff, 0, nBuffSize);

			DWORD dwReads;
			hr = fSrc.Read(pBlock->buff, nBuffSize, dwReads);
			if (FAILED(hr) || dwReads == 0)
				break;
			pBlock->size = (uint16_t)dwReads;
			pBlock->crc = CalcBlockCRC(pBlock, nBlockSize);

			hr = fDst.Write(pBlock, nBlockSize);
			if (FAILED(hr))
				break;
		}

		if (FAILED(hr))
			break;

	} while (FALSE);

	if (bDelOld)
	{
		::DeleteFile(wsFileNoTag);
	}

	if (FAILED(hr))
		::DeleteFile(wsFileTag);

	return hr;
}

HRESULT CFileClusterTag::RemoveTag(__in LPCWSTR wsFileTag, __in LPCWSTR wsFileNewDir/* = NULL*/, __inout LPWSTR wsFileSrc/* = NULL*/, __in DWORD dwBufBytes/* = 0*/, __in BOOL bDelOld/* = TRUE*/)
{
	HRESULT hr = E_FAIL;

	do
	{
		CAtlFile fSrc;
		hr = fSrc.Create(wsFileTag, GENERIC_READ, 0, OPEN_EXISTING);
		if (FAILED(hr))
			break;

		ULONGLONG ullen = 0;
		fSrc.GetSize(ullen);

		int nBlockSize = GetBytesPerCluster(wsFileTag);;		// 先假设簇大小是平台大小
		int nBuffSize = nBlockSize - SAFEBK_HDR_SIZE;
		CAtlFile fDst;

		int nCacheMaxSize = 64 * 1024;
		if (nCacheMaxSize < nBlockSize)
			nCacheMaxSize = nBlockSize;
		int nCacheSize = ullen > nCacheMaxSize ? nCacheMaxSize : ullen;		// 64KB读写，性能最佳
		char *bfCache = new char[nCacheSize];		CAutoPtr<char> _auto_free1(bfCache);
		memset(bfCache, 0, nCacheSize);

		BOOL bFirst = TRUE;
		BOOL bCheckClusterSize = FALSE;
		while (1)
		{
			DWORD dwBytesRead = 0;
			hr = fSrc.Read(bfCache, nCacheSize, dwBytesRead);
			if (FAILED(hr) || dwBytesRead == 0)
				break;

			PSAFEBK_BLOCK_HDR pBlock = (PSAFEBK_BLOCK_HDR)(bfCache);

			// 先确认下，簇大小是否是正确的，不是的话，打回重新来，并且更正簇大小
			if (!bCheckClusterSize)
			{
				bCheckClusterSize = TRUE;
				if (pBlock->clustersize != nBlockSize)
				{
					nBlockSize = pBlock->clustersize;
					nBuffSize = nBlockSize - SAFEBK_HDR_SIZE;

					if (nCacheMaxSize < nBlockSize)
					{
						nCacheSize = ullen > nCacheMaxSize ? nCacheMaxSize : ullen;
						bfCache = new char[nCacheSize];
						_auto_free1.Attach(bfCache);
						memset(bfCache, 0, nCacheSize);
					}

					fSrc.Seek(0, FILE_BEGIN);
					continue;
				}
			}

			int nCnt = dwBytesRead / nBlockSize + ((dwBytesRead % nBlockSize) == 0 ? 0 : 1);

			for (size_t i = 0; i < nCnt; i++)
			{
				if (bFirst)
				{
					// 取第一个块的内容
					bFirst = FALSE;

					// 校验数据块
					if (memcmp(&pBlock->_head, &GUID_SAFEBK_ID, sizeof(GUID)) != 0)
						break;
					if (memcmp(&pBlock->_tail, &GUID_SAFEBK_ID, sizeof(GUID)) != 0)
						break;
					if (pBlock->version != 1)
						break;
					if (pBlock->size > nBlockSize)
						break;
					if (pBlock->crc != CalcBlockCRC(pBlock, nBlockSize))
						break;

					PSAFEBK_FIRST_BLOCK pInfo = (PSAFEBK_FIRST_BLOCK)pBlock->buff;
					if (pInfo->filenamelen == 0)
						break;

					WCHAR* pFilePath = new WCHAR[pInfo->filenamelen / 2 + 1];		CAutoPtr<WCHAR> _auto_free2(pFilePath);
					memset(pFilePath, 0, pInfo->filenamelen + 2);
					memcpy(pFilePath, pInfo->filename, pInfo->filenamelen);

					if (wsFileSrc && dwBufBytes > pInfo->filenamelen)
					{
						memcpy(wsFileSrc, pInfo->filename, pInfo->filenamelen);
					}

					WCHAR fileNewPath[1024] = { 0 };

					if (wsFileNewDir)
					{
						// 存储到新目录下
						WCHAR fileName[256] = { 0 };
						wcscpy(fileName, PathFindFileName(pFilePath));
						wsprintf(fileNewPath, L"%s%s", wsFileNewDir, fileName);

						pFilePath = fileNewPath;
					}

					hr = fDst.Create(pFilePath, GENERIC_WRITE, 0, CREATE_ALWAYS);
					if (FAILED(hr))
						break;

					continue;
				}

				PSAFEBK_BLOCK_HDR pBlock = (PSAFEBK_BLOCK_HDR)(bfCache + i * nBlockSize);

				// 校验数据块
				if (memcmp(&pBlock->_head, &GUID_SAFEBK_ID, sizeof(GUID)) != 0)
					break;
				if (memcmp(&pBlock->_tail, &GUID_SAFEBK_ID, sizeof(GUID)) != 0)
					break;
				if (pBlock->version != 1)
					break;
				if (pBlock->size > nBlockSize)
					break;
				if (pBlock->crc != CalcBlockCRC(pBlock, nBlockSize))
					break;

				fDst.Write(pBlock->buff, pBlock->size);
			}
		}

	} while (FALSE);

	return S_OK;
}

HRESULT CFileClusterTag::DiskRestore(LPCWSTR wsDevName, ULONGLONG llScanBegin, ULONGLONG llScanEnd)
{
	HRESULT hr;

	WCHAR wsTmp[100];

	int nBlockSize = GetBytesPerCluster(wsDevName);
	int nBuffSize = nBlockSize - SAFEBK_HDR_SIZE;
	char *bfBlock = new char[nBlockSize];	CAutoPtr<char> _auto_free1(bfBlock);
	memset(bfBlock, 0, nBlockSize);

	int nCacheBuffSize = 10 * 1024 * 1024;
	char *bfCache = new char[nCacheBuffSize];	CAutoPtr<char> _auto_free2(bfCache);
	memset(bfCache, 0, nCacheBuffSize);

	CAtlFile f;
	hr = f.Create(wsDevName, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, OPEN_EXISTING, FILE_FLAG_NO_BUFFERING | FILE_FLAG_RANDOM_ACCESS);
	if (FAILED(hr))
		return hr;

	hr = f.Seek(llScanBegin, FILE_BEGIN);
	if (FAILED(hr))
		return hr;

	for (ULONGLONG ullPos = llScanBegin; ullPos < llScanEnd; ullPos += nCacheBuffSize)
	{
		DWORD dwReads = 0;
		hr = f.Read(bfCache, nCacheBuffSize, dwReads);
		if (FAILED(hr) || dwReads == 0)
			break;

		int nCnt = dwReads / 512;
		for (int j = 0; j < nCnt; j++)
		{
			PSAFEBK_BLOCK_HDR pBlock = (PSAFEBK_BLOCK_HDR)(bfCache + j * 512);
			ULONGLONG ullCurrOffset = ullPos + j * 512;
			int nCacheSize = nCacheBuffSize - j * 512;

			if (memcmp(&pBlock->_head, &GUID_SAFEBK_ID, sizeof(GUID)) != 0)
				continue;
			if (memcmp(&pBlock->_tail, &GUID_SAFEBK_ID, sizeof(GUID)) != 0)
				continue;

			if (nCacheSize < nBlockSize)
			{
				hr = f.Seek(ullCurrOffset, FILE_BEGIN);
				if (FAILED(hr))
					break;
				hr = f.Read(bfBlock, nBlockSize, dwReads);
				if (FAILED(hr) || dwReads == 0)
					break;
				pBlock = (PSAFEBK_BLOCK_HDR)bfBlock;
			}

			if (pBlock->version != 1)
				continue;

			if (pBlock->crc != CalcBlockCRC(pBlock, nBlockSize))
				continue;

			StringFromGUID2(pBlock->fileid, wsTmp, _countof(wsTmp));
			wprintf(L"block pos: %p, fid: %s, curr: %lld, total: %lld, size: %d\n", ullCurrOffset, wsTmp, pBlock->curr, pBlock->total, pBlock->size);
		}
	}

	if (FAILED(hr))
		return hr;
	return hr;
}

