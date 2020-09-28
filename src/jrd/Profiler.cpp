/*
 *  The contents of this file are subject to the Initial
 *  Developer's Public License Version 1.0 (the "License");
 *  you may not use this file except in compliance with the
 *  License. You may obtain a copy of the License at
 *  http://www.ibphoenix.com/main.nfs?a=ibphoenix&page=ibp_idpl.
 *
 *  Software distributed under the License is distributed AS IS,
 *  WITHOUT WARRANTY OF ANY KIND, either express or implied.
 *  See the License for the specific language governing rights
 *  and limitations under the License.
 *
 *  The Original Code was created by Adriano dos Santos Fernandes
 *  for the Firebird Open Source RDBMS project.
 *
 *  Copyright (c) 2020 Adriano dos Santos Fernandes <adrianosf@gmail.com>
 *  and all contributors signed below.
 *
 *  All Rights Reserved.
 *  Contributor(s): ______________________________________.
 */

#include "firebird.h"
#include "../jrd/Profiler.h"
#include "../jrd/Record.h"
#include "../jrd/ini.h"
#include "../jrd/tra.h"
#include "../jrd/ids.h"

using namespace Jrd;
using namespace Firebird;


ProfileSnapshotData::ProfileSnapshotData(thread_db* tdbb)
	: SnapshotData(*tdbb->getAttachment()->att_pool),
	  pool(*tdbb->getAttachment()->att_pool)
{
	allocBuffers(tdbb);
}

void ProfileSnapshotData::reset(thread_db* tdbb)
{
	clearSnapshot();
	allocBuffers(tdbb);
}

void ProfileSnapshotData::update(thread_db* tdbb, Profiler* profiler)
{
	const auto sessionBuffer = getData(rel_prof_sessions);
	const auto requestBuffer = getData(rel_prof_requests);
	const auto statsBuffer = getData(rel_prof_stats);

	const auto sessionRecord = sessionBuffer->getTempRecord();
	const auto requestRecord = requestBuffer->getTempRecord();
	const auto statsRecord = statsBuffer->getTempRecord();

	sessionBuffer->resetCount(profiler->previousSnapshotCounters.sessions);
	requestBuffer->resetCount(profiler->previousSnapshotCounters.requests);
	statsBuffer->resetCount(profiler->previousSnapshotCounters.stats);

	auto sessionAccessor = profiler->sessions.accessor();

	for (bool sessionFound = sessionAccessor.getFirst(); sessionFound;)
	{
		const SINT64 sessionId = sessionAccessor.current()->first;
		const auto& sessionDescription = sessionAccessor.current()->second.description;
		const ISC_TIMESTAMP_TZ sessionStartTimeStamp = sessionAccessor.current()->second.startTimeStamp;
		const Nullable<ISC_TIMESTAMP_TZ> sessionFinishTimeStamp = sessionAccessor.current()->second.finishTimeStamp;

		sessionRecord->nullify();
		putField(tdbb, sessionRecord, DumpField(f_prof_ses_id, VALUE_INTEGER, sizeof(sessionId), &sessionId));

		if (sessionDescription.hasData())
		{
			putField(tdbb, sessionRecord, DumpField(f_prof_ses_desc, VALUE_STRING,
				sessionDescription.length(), sessionDescription.c_str()));
		}

		putField(tdbb, sessionRecord, DumpField(f_prof_ses_start_timestamp, VALUE_TIMESTAMP_TZ,
			sizeof(sessionStartTimeStamp), &sessionStartTimeStamp));

		if (sessionFinishTimeStamp.isAssigned())
		{
			putField(tdbb, sessionRecord, DumpField(f_prof_ses_finish_timestamp, VALUE_TIMESTAMP_TZ,
				sizeof(sessionFinishTimeStamp.value), &sessionFinishTimeStamp.value));
		}

		sessionBuffer->store(sessionRecord);

		for (const auto& requestIt : sessionAccessor.current()->second.requests)
		{
			const SINT64 requestId = requestIt.first;
			const auto& profileRequest = requestIt.second;
			const ISC_TIMESTAMP_TZ requestTimeStamp = profileRequest.timeStamp;

			requestRecord->nullify();
			putField(tdbb, requestRecord, DumpField(f_prof_req_ses_id, VALUE_INTEGER, sizeof(sessionId), &sessionId));
			putField(tdbb, requestRecord, DumpField(f_prof_req_time, VALUE_TIMESTAMP_TZ,
				sizeof(requestTimeStamp), &requestTimeStamp));
			putField(tdbb, requestRecord, DumpField(f_prof_req_req_id, VALUE_INTEGER, sizeof(requestId), &requestId));
			putField(tdbb, requestRecord, DumpField(f_prof_req_type, VALUE_STRING,
				profileRequest.requestType.length(), profileRequest.requestType.c_str()));

			if (profileRequest.packageName.hasData())
			{
				putField(tdbb, requestRecord, DumpField(f_prof_req_pkg_name, VALUE_STRING,
					profileRequest.packageName.length(), profileRequest.packageName.c_str()));
			}

			if (profileRequest.routineName.hasData())
			{
				putField(tdbb, requestRecord, DumpField(f_prof_req_routine, VALUE_STRING,
					profileRequest.routineName.length(), profileRequest.routineName.c_str()));
			}

			if (profileRequest.sqlText.hasData())
			{
				putField(tdbb, requestRecord, DumpField(f_prof_req_sql_text, VALUE_STRING,
					profileRequest.sqlText.length(), profileRequest.sqlText.c_str()));
			}

			requestBuffer->store(requestRecord);

			for (const auto& statsIt : profileRequest.stats)
			{
				const auto lineColumn = Profiler::decodeLineColumn(statsIt.first);
				const SINT64 line = lineColumn.first;
				const SINT64 column = lineColumn.second;
				const SINT64 counter = statsIt.second.counter;
				const SINT64 minTime = statsIt.second.minTime;
				const SINT64 maxTime = statsIt.second.maxTime;
				const SINT64 totalTime = statsIt.second.totalTime;

				statsRecord->nullify();
				putField(tdbb, statsRecord, DumpField(f_prof_stats_ses_id, VALUE_INTEGER, sizeof(sessionId), &sessionId));
				putField(tdbb, statsRecord, DumpField(f_prof_stats_req_id, VALUE_INTEGER, sizeof(requestId), &requestId));
				putField(tdbb, statsRecord, DumpField(f_prof_stats_line, VALUE_INTEGER, sizeof(line), &line));
				putField(tdbb, statsRecord, DumpField(f_prof_stats_column, VALUE_INTEGER, sizeof(column), &column));
				putField(tdbb, statsRecord, DumpField(f_prof_stats_counter, VALUE_INTEGER, sizeof(counter), &counter));
				putField(tdbb, statsRecord, DumpField(f_prof_stats_min_time, VALUE_INTEGER, sizeof(minTime), &minTime));
				putField(tdbb, statsRecord, DumpField(f_prof_stats_max_time, VALUE_INTEGER, sizeof(maxTime), &maxTime));
				putField(tdbb, statsRecord, DumpField(f_prof_stats_total_time, VALUE_INTEGER, sizeof(totalTime), &totalTime));
				statsBuffer->store(statsRecord);
			}
		}

		if (sessionId != profiler->currentSessionId)
		{
			profiler->previousSnapshotCounters.sessions = sessionBuffer->getCount();
			profiler->previousSnapshotCounters.requests = requestBuffer->getCount();
			profiler->previousSnapshotCounters.stats = statsBuffer->getCount();

			sessionFound = sessionAccessor.fastRemove();
		}
		else
			sessionFound = sessionAccessor.getNext();
	}
}

void ProfileSnapshotData::allocBuffers(thread_db* tdbb)
{
	allocBuffer(tdbb, pool, rel_prof_sessions);
	allocBuffer(tdbb, pool, rel_prof_requests);
	allocBuffer(tdbb, pool, rel_prof_stats);
}


//--------------------------------------


const Format* ProfileTableScan::getFormat(thread_db* tdbb, jrd_rel* relation) const
{
	const auto profiler = tdbb->getAttachment()->getProfiler(tdbb);
	return profiler->snapshotData.getData(relation)->getFormat();
}

bool ProfileTableScan::retrieveRecord(thread_db* tdbb, jrd_rel* relation,
	FB_UINT64 position, Record* record) const
{
	const auto profiler = tdbb->getAttachment()->getProfiler(tdbb);
	return profiler->snapshotData.getData(relation)->fetch(position, record);
}


//--------------------------------------


IExternalResultSet* ProfilerPackage::updateSnapshotProcedure(ThrowStatusExceptionWrapper* status,
	IExternalContext* context, const void* in, void* out)
{
	const auto tdbb = JRD_get_thread_data();
	const auto attachment = tdbb->getAttachment();

	Attachment::SyncGuard guard(attachment, FB_FUNCTION);

	const auto profiler = attachment->getProfiler(tdbb);

	profiler->snapshotData.update(tdbb, profiler);

	return nullptr;
}

IExternalResultSet* ProfilerPackage::finishSessionProcedure(ThrowStatusExceptionWrapper* status,
	IExternalContext* context, const FinishSessionInput::Type* in, void* out)
{
	const auto tdbb = JRD_get_thread_data();
	const auto attachment = tdbb->getAttachment();

	Attachment::SyncGuard guard(attachment, FB_FUNCTION);

	const auto profiler = attachment->getProfiler(tdbb);

	if (profiler->activeSession)
	{
		profiler->activeSession = false;

		const auto profileSession = profiler->sessions.get(profiler->currentSessionId);

		profileSession->finishTimeStamp = TimeZoneUtil::getCurrentSystemTimeStamp();
		profileSession->finishTimeStamp.value.time_zone = attachment->att_current_timezone;
	}

	if (in->updateSnapshot)
		profiler->snapshotData.update(tdbb, profiler);

	return nullptr;
}

IExternalResultSet* ProfilerPackage::pauseSessionProcedure(ThrowStatusExceptionWrapper* status,
	IExternalContext* context, const PauseSessionInput::Type* in, void* out)
{
	const auto tdbb = JRD_get_thread_data();
	const auto attachment = tdbb->getAttachment();

	Attachment::SyncGuard guard(attachment, FB_FUNCTION);

	const auto profiler = attachment->getProfiler(tdbb);

	if (!profiler->activeSession)
		return nullptr;

	profiler->paused = true;

	if (in->updateSnapshot)
		profiler->snapshotData.update(tdbb, profiler);

	return nullptr;
}

IExternalResultSet* ProfilerPackage::purgeSnapshotsProcedure(ThrowStatusExceptionWrapper* status,
	IExternalContext* context, const void* in, void* out)
{
	const auto tdbb = JRD_get_thread_data();
	const auto attachment = tdbb->getAttachment();

	Attachment::SyncGuard guard(attachment, FB_FUNCTION);

	const auto profiler = attachment->getProfiler(tdbb);

	profiler->snapshotData.reset(tdbb);
	profiler->previousSnapshotCounters = {};

	auto sessionAccessor = profiler->sessions.accessor();

	for (bool sessionFound = sessionAccessor.getFirst(); sessionFound;)
	{
		const auto sessionId = sessionAccessor.current()->first;

		if (!profiler->activeSession || sessionId != profiler->currentSessionId)
			sessionFound = sessionAccessor.fastRemove();
		else
			sessionFound = sessionAccessor.getNext();
	}

	return nullptr;
}

IExternalResultSet* ProfilerPackage::resumeSessionProcedure(ThrowStatusExceptionWrapper* status,
	IExternalContext* context, const void* in, void* out)
{
	const auto tdbb = JRD_get_thread_data();
	const auto attachment = tdbb->getAttachment();

	Attachment::SyncGuard guard(attachment, FB_FUNCTION);

	const auto profiler = attachment->getProfiler(tdbb);

	if (profiler->activeSession && profiler->paused)
		profiler->paused = false;

	return nullptr;
}

void ProfilerPackage::startSessionFunction(ThrowStatusExceptionWrapper* status,
	IExternalContext* context, const StartSessionInput::Type* in, StartSessionOutput::Type* out)
{
	const auto tdbb = JRD_get_thread_data();
	const auto attachment = tdbb->getAttachment();

	Attachment::SyncGuard guard(attachment, FB_FUNCTION);

	const auto profiler = attachment->getProfiler(tdbb);

	profiler->activeSession = true;
	profiler->paused = false;

	const auto sessionId = ++profiler->currentSessionId;

	profiler->sessions.put(sessionId)->init(attachment,
		in->descriptionNull ? "" : string(string(in->description.str, in->description.length)));

	out->sessionIdNull = FB_FALSE;
	out->sessionId = sessionId;
}


//--------------------------------------


void Profiler::Request::init(jrd_req* request)
{
	timeStamp.time_zone = request->req_attachment->att_current_timezone;

	if (request->getStatement()->sqlText.hasData())
		sqlText = *request->getStatement()->sqlText;
}


void Profiler::Session::init(Attachment* attachment, const string& aDescription)
{
	startTimeStamp.time_zone = attachment->att_current_timezone;
	description = aDescription;
}

//--------------------------------------


Profiler::Profiler(thread_db* tdbb)
	: sessions(*tdbb->getAttachment()->att_pool),
	  snapshotData(tdbb)
{
}

Profiler* Profiler::create(thread_db* tdbb)
{
	return FB_NEW_POOL(*tdbb->getAttachment()->att_pool) Profiler(tdbb);
}

void Profiler::hitLineColumn(jrd_req* request, ULONG line, ULONG column, SINT64 runTime)
{
	if (!isActive())
		return;

	const auto profileSession = sessions.get(currentSessionId);
	fb_assert(profileSession);

	auto profileRequest = profileSession->requests.get(request->getRequestId());

	if (!profileRequest)
	{
		profileRequest = profileSession->requests.put(request->getRequestId());
		profileRequest->init(request);

		if (request->getStatement()->getRoutine())
		{
			if (request->getStatement()->procedure)
				profileRequest->requestType = "PROCEDURE";
			else if (request->getStatement()->function)
				profileRequest->requestType = "FUNCTION";

			profileRequest->packageName =
				request->getStatement()->getRoutine()->getName().package;

			profileRequest->routineName =
				request->getStatement()->getRoutine()->getName().identifier;
		}
		else if (request->getStatement()->triggerName.hasData())
		{
			profileRequest->requestType = "TRIGGER";
			profileRequest->routineName = request->getStatement()->triggerName;
		}
		else
			profileRequest->requestType = "BLOCK";
	}

	const auto statsKey = encodeLineColumn(line, column);
	auto profileStats = profileRequest->stats.get(statsKey);

	if (!profileStats)
		profileStats = profileRequest->stats.put(statsKey);

	if (profileStats->counter == 0 || runTime < profileStats->minTime)
		profileStats->minTime = runTime;

	if (profileStats->counter == 0 || runTime > profileStats->maxTime)
		profileStats->maxTime = runTime;

	profileStats->totalTime += runTime;
	++profileStats->counter;
}

//--------------------------------------


ProfilerPackage::ProfilerPackage(MemoryPool& pool)
	: SystemPackage(
		pool,
		"RDB$PROFILER",
		ODS_13_0,	//// TODO: adjust
		// procedures
		{
			SystemProcedure(
				pool,
				"FINISH_SESSION",
				SystemProcedureFactory<FinishSessionInput, VoidMessage, finishSessionProcedure>(),
				prc_executable,
				// input parameters
				{
					{"UPDATE_SNAPSHOT", fld_bool, false}
				},
				// output parameters
				{
				}
			),
			SystemProcedure(
				pool,
				"UPDATE_SNAPSHOT",
				SystemProcedureFactory<VoidMessage, VoidMessage, updateSnapshotProcedure>(),
				prc_executable,
				// input parameters
				{
				},
				// output parameters
				{
				}
			),
			SystemProcedure(
				pool,
				"PAUSE_SESSION",
				SystemProcedureFactory<PauseSessionInput, VoidMessage, pauseSessionProcedure>(),
				prc_executable,
				// input parameters
				{
					{"UPDATE_SNAPSHOT", fld_bool, false}
				},
				// output parameters
				{
				}
			),
			SystemProcedure(
				pool,
				"PURGE_SNAPSHOTS",
				SystemProcedureFactory<VoidMessage, VoidMessage, purgeSnapshotsProcedure>(),
				prc_executable,
				// input parameters
				{
				},
				// output parameters
				{
				}
			),
			SystemProcedure(
				pool,
				"RESUME_SESSION",
				SystemProcedureFactory<VoidMessage, VoidMessage, resumeSessionProcedure>(),
				prc_executable,
				// input parameters
				{
				},
				// output parameters
				{
				}
			)
		},
		// functions
		{
			SystemFunction(
				pool,
				"START_SESSION",
				SystemFunctionFactory<StartSessionInput, StartSessionOutput, startSessionFunction>(),
				// parameters
				{
					{"DESCRIPTION", fld_short_description, true}
				},
				{fld_prof_ses_id, false}
			)
		}
	)
{
}
