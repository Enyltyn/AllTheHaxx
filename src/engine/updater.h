#ifndef ENGINE_UPDATER_H
#define ENGINE_UPDATER_H

#include "kernel.h"

class IUpdater : public IInterface
{
	MACRO_INTERFACE("updater", 0)
public:
	enum
	{
		CLEAN = 0,
		GETTING_MANIFEST,
		GOT_MANIFEST,
		PARSING_UPDATE,
		DOWNLOADING,
		MOVE_FILES,
		NEED_RESTART,
		FAIL,
	};

	virtual void Update() = 0;
	virtual void InitiateUpdate(bool CheckOnly = false, bool ForceRefresh = false) = 0;

	virtual const char *GetLatestVersion() const = 0;
	virtual const char *GetNews() const = 0;
	virtual const char *GetFailedFile() const = 0;

	virtual int GetCurrentState() const = 0;
	virtual char *GetCurrentFile() = 0;
	virtual int GetCurrentPercent() const = 0;
};

#endif
