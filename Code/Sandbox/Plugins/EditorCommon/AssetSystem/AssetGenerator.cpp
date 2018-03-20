// Copyright 2001-2016 Crytek GmbH. All rights reserved.

#include "StdAfx.h"
#include "AssetGenerator.h"
#include "Loader/AssetLoaderBackgroundTask.h"
#include "AssetSystem/AssetManager.h"
#include "Notifications/NotificationCenter.h"
#include "FilePathUtil.h"
#include "ThreadingUtils.h"
#include "QtUtil.h"

#include <CrySystem/IProjectManager.h>
#include <CryString/CryPath.h>

namespace Private_AssetGenerator
{

class CBatchProcess : public CProgressNotification
{
public: 
	CBatchProcess()
		: CProgressNotification(tr("Generating asset metadata"), QString(), true)
		, m_totalItemsCount(0)
		, m_precessedItemsCount(0)
	{
	}

	void PushItem() { ++m_totalItemsCount; }
	void PopItem() { ++m_precessedItemsCount; }
	bool IsDone() {	return m_totalItemsCount == m_precessedItemsCount; }
	virtual void ShowProgress(const string& filePath)
	{
		const QString msg = QtUtil::ToQString(PathUtil::GetFile(filePath));
		const size_t  precessedItemsCount = m_precessedItemsCount.load();
		const size_t  totalItemsCount = m_totalItemsCount.load();
		const float progress = float(precessedItemsCount) / totalItemsCount;
		SetMessage(msg);
		SetProgress(progress);
	}

private:
	std::atomic<size_t> m_totalItemsCount;
	std::atomic<size_t> m_precessedItemsCount;
};

}

namespace AssetManagerHelpers
{

void CAssetGenerator::RegisterFileListener()
{
	static CAssetGenerator theInstance;
}

// IFileChangeListener implementation.
void CAssetGenerator::OnFileChange(const char* szFilename, EChangeType changeType)
{
	// Ignore events for files outside of the current game folder.
	if (GetISystem()->GetIPak()->IsAbsPath(szFilename))
	{
		return;
	}

	if (changeType != IFileChangeListener::eChangeType_Created
	    && changeType != IFileChangeListener::eChangeType_RenamedNewName
	    && changeType != IFileChangeListener::eChangeType_Modified)
	{
		return;
	}

	// Ignore files that start with a dot.
	if (szFilename[0] == '.')
	{
		return;
	}

	const string cryasset = string().Format("%s.cryasset", szFilename).MakeLower();

	// Refresh cryasset files for the following types even if exists. 
	// These asset types do not have true asset editors to update cryasset files.
	static const char* const update[] = { "lua", "xml", "mtl", "cdf" };

	const char* szExt = PathUtil::GetExt(szFilename);

	const bool  bUpdate = std::any_of(std::begin(update), std::end(update), [szExt](const char* szUpdatable)
	{
		return stricmp(szExt, szUpdatable) == 0;
	});

	if (!bUpdate && GetISystem()->GetIPak()->IsFileExist(cryasset))
	{
		return;
	}

	const char* const szAssetDirectory = GetIEditor()->GetProjectManager()->GetCurrentAssetDirectoryAbsolute();
	const string filePath = PathUtil::Make(szAssetDirectory, szFilename);

	m_fileQueue.ProcessItemUniqueAsync(filePath, [this](const string& path)
	{
		// It can be that the file is still being opened for writing.
		if (IsFileOpened(path))
		{
			// Try again
			return false;
		}

		GenerateCryasset(path);
		return true;
	});
}

bool CAssetGenerator::GenerateCryassets()
{
	const string jobFile = PathUtil::Make(PathUtil::GetEnginePath(), "tools/cryassets/rcjob_cryassets.xml");
	const string options = string().Format("/job=\"%s\" /src=\"%s\"", jobFile.c_str(), PathUtil::GetGameProjectAssetsPath().c_str());

	RCLogger rcLogger;

	return CResourceCompilerHelper::ERcCallResult::eRcCallResult_success == CResourceCompilerHelper::CallResourceCompiler(
		nullptr,
		options.c_str(),
		&rcLogger,
		false, // may show window?
		CResourceCompilerHelper::eRcExePath_editor,
		true,  // silent?
		true); // no user dialog?
}

CAssetGenerator::CAssetGenerator()
{
	const std::vector<CAssetType*>& types = CAssetManager::GetInstance()->GetAssetTypes();

	m_rcSettings.reserve(types.size() * 20);
	m_rcSettings.Append("/overwriteextension=cryasset /assettypes=\"");
	for (CAssetType* pType : types)
	{
		// Ignore fallback asset type.
		if (strcmp(pType->GetTypeName(), "cryasset") == 0)
		{
			continue;
		}

		// Ignore levels, since this is a special case when the cryasset is next to the level folder.
		if (strcmp(pType->GetTypeName(), "Level") == 0)
		{
			continue;
		}

		m_rcSettings.AppendFormat("%s,%s;", pType->GetFileExtension(), pType->GetTypeName());
		GetIEditor()->GetFileMonitor()->RegisterListener(this, "", pType->GetFileExtension());
	}
	m_rcSettings.Append("\"");

	// TODO: There are .wav.cryasset and .ogg.cryasset for the CSoundType. Remove the following scoped lines when this is fixed.
	{
		m_rcSettings.Append("ogg,Sound;");
		GetIEditor()->GetFileMonitor()->RegisterListener(this, "", "ogg");
	}

	m_rcSettings.shrink_to_fit();
}

void CAssetGenerator::GenerateCryasset(const string& filePath)
{
	using namespace Private_AssetGenerator;

	if (!m_pProgress)
	{
		m_pProgress.reset(new CBatchProcess());
	}
	static_cast<CBatchProcess*>(m_pProgress.get())->PushItem();

	ThreadingUtils::AsyncQueue([filePath, this]()
	{
		RCLogger rcLogger;

		CBatchProcess* pProgress = static_cast<CBatchProcess*>(m_pProgress.get());
		CRY_ASSERT(pProgress);

		pProgress->ShowProgress(filePath);

		CResourceCompilerHelper::CallResourceCompiler(
			filePath.c_str(),
			m_rcSettings.c_str(),
			&rcLogger,
			false, // may show window?
			CResourceCompilerHelper::eRcExePath_editor,
			true,  // silent?
			true);

			ThreadingUtils::PostOnMainThread([this]() 
			{
				// It mustn't be null here by design.
				CBatchProcess* pProgress = static_cast<CBatchProcess*>(m_pProgress.get());
				CRY_ASSERT(pProgress); 

				pProgress->PopItem();
				if (pProgress->IsDone())
				{
					m_pProgress.reset(nullptr);
				}
			});
	});
}

}