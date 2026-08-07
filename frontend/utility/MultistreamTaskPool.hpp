/******************************************************************************
    Copyright (C) 2026

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#pragma once

#include <QThreadPool>

/* OAuth work uses blocking libcurl calls with a 30 s timeout. Running those on
 * the global pool would starve the rest of the application, so account
 * refreshes and ingest resolution get a small pool of their own. */
inline QThreadPool &MultistreamTaskPool()
{
	static QThreadPool pool;
	static bool configured = [] {
		pool.setMaxThreadCount(3);
		pool.setObjectName(QStringLiteral("MultistreamTaskPool"));
		return true;
	}();
	Q_UNUSED(configured);
	return pool;
}
