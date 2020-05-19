//===== Copyright � 1996-2006, Valve Corporation, All rights reserved. ======//
//
// Purpose: The thread which performs lighting preview
//
//===========================================================================//

#include "stdafx.h"
#include "lpreview_thread.h"
#include "mathlib/simdvectormatrix.h"
#include "raytrace.h"
#include "hammer.h"
#include "mainfrm.h"
#include "mapdoc.h"
#include "lprvwindow.h"
#include "vstdlib/jobthread.h"

// memdbgon must be the last include file in a .cpp file!!!
#include <tier0/memdbgon.h>

CInterlockedInt n_gbufs_queued;

// the current lighting preview output, if we have one
Bitmap_t *g_pLPreviewOutputBitmap;

enum IncrementalLightState
{
	INCR_STATE_NO_RESULTS=0,							// we threw away the results for this light
	INCR_STATE_PARTIAL_RESULTS=1,							// have done some but not all
	INCR_STATE_NEW=2,										// we know nothing about this light
	INCR_STATE_HAVE_FULL_RESULTS=3,							// we are done
};


class CLightingPreviewThread;

class CIncrementalLightInfo
{
public:
	CIncrementalLightInfo*				m_pNext;
	CLightingPreviewLightDescription*	m_pLight;
	// incremental lighting tracking information
	int									m_nObjectID;
	int									m_PartialResultsStage;
	IncrementalLightState				m_eIncrState;
	CSIMDVectorMatrix					m_CalculatedContribution;
	float								m_fTotalContribution;		// current magnitude of light effect
	float								m_fDistanceToEye;
	int									m_nMostRecentNonZeroContributionTimeStamp;

	CIncrementalLightInfo()
	{
		m_pNext = nullptr;
		m_pLight = nullptr;
		m_nObjectID = -1;
		m_PartialResultsStage = 0;
		m_eIncrState = INCR_STATE_NEW;
		m_fTotalContribution = 0.f;
		m_fDistanceToEye = 0.f;
		m_nMostRecentNonZeroContributionTimeStamp = 0;
	}

	void DiscardResults()
	{
		m_CalculatedContribution.SetSize( 0, 0 );
		if ( m_eIncrState != INCR_STATE_NEW )
			m_eIncrState = INCR_STATE_NO_RESULTS;
	}

	void ClearIncremental()
	{
		m_eIncrState = INCR_STATE_NEW;
		// free calculated lighting matrix
		DiscardResults();
	}

	bool HasWorkToDo() const
	{
		return m_eIncrState != INCR_STATE_HAVE_FULL_RESULTS;
	}


	bool IsLowerPriorityThan( CLightingPreviewThread* pLPV, CIncrementalLightInfo const& other ) const;

	bool IsHighPriority( CLightingPreviewThread* pLPV ) const;
};

static constexpr int N_INCREMENTAL_STEPS = 32;

class CLightingPreviewThread
{
public:
	CUtlVector<CLightingPreviewLightDescription>*	m_pLightList;

	CSIMDVectorMatrix								m_Positions;
	CSIMDVectorMatrix								m_Normals;
	CSIMDVectorMatrix								m_Albedos;
	CSIMDVectorMatrix								m_ResultImage;

	RayTracingEnvironment*							m_pRtEnv;
	CIncrementalLightInfo*							m_pIncrementalLightInfoList;

	bool											m_bAccStructureBuilt;
	Vector											m_LastEyePosition;

	bool											m_bResultChangedSinceLastSend;
	float											m_fLastSendTime;

	int												m_LineMask[N_INCREMENTAL_STEPS];
	int												m_ClosestLineOffset[N_INCREMENTAL_STEPS][N_INCREMENTAL_STEPS];
	int												m_nBitmapGenerationCounter;
	int												m_nContributionCounter;

	// bounidng box of the rendered scene+ the eye
	Vector											m_MinViewCoords;
	Vector											m_MaxViewCoords;

	CLightingPreviewThread()
	{
		m_nBitmapGenerationCounter = -1;
		m_pLightList = nullptr;
		m_pRtEnv = nullptr;
		m_bAccStructureBuilt = false;
		m_pIncrementalLightInfoList = nullptr;
		m_fLastSendTime = -1.0e6;
		m_bResultChangedSinceLastSend = false;
		m_nContributionCounter = 1000000;
		InitIncrementalInformation();
	}

	void InitIncrementalInformation();

	~CLightingPreviewThread()
	{
		if ( m_pLightList )
			delete m_pLightList;
		while ( m_pIncrementalLightInfoList )
		{
			CIncrementalLightInfo *n=m_pIncrementalLightInfoList->m_pNext;
			delete m_pIncrementalLightInfoList;
			m_pIncrementalLightInfoList = n;
		}
	}

	// check if the master has new work for us to do, meaning we should abort rendering
	bool ShouldAbort()
	{
		return g_HammerToLPreviewMsgQueue.MessageWaiting();
	}

	// main loop
	void Run();

	// handle new g-buffers from master
	void HandleGBuffersMessage( MessageToLPreview& msg_in );

	// accept triangle list from master
	void HandleGeomMessage( MessageToLPreview& msg_in );

	// send one of our output images back
	void SendVectorMatrixAsRendering( CSIMDVectorMatrix const& src );

	// calculate m_MinViewCoords, m_MaxViewCoords - the bounding box of the rendered pixels+the eye
	void CalculateSceneBounds();

	// inner lighting loop. meant to be multithreaded on dual-core (or more)
	void CalculateForLightTask( int nLineMask, int nLineMatch, const CLightingPreviewLightDescription& l, int calc_mask, float* fContributionOut );

	void CalculateForLight( const CLightingPreviewLightDescription& l );

	// send our current output back
	void SendResult();

	void UpdateIncrementalForNewLightList();

	void DiscardResults()
	{
		// invalidate all per light result data
		for ( CIncrementalLightInfo* i = m_pIncrementalLightInfoList; i; i = i->m_pNext )
			i->DiscardResults();

		// bump time stamp
		m_nContributionCounter++;
		// update distances to lights
		if ( m_pLightList )
			for ( int i = 0; i < m_pLightList->Count(); i++ )
			{
				CLightingPreviewLightDescription& l = ( *m_pLightList )[i];
				CIncrementalLightInfo* l_info = l.m_pIncrementalInfo;
				if ( l.m_Type == MATERIAL_LIGHT_DIRECTIONAL )
					l_info->m_fDistanceToEye = 0;			// high priority
				else
					l_info->m_fDistanceToEye = m_LastEyePosition.DistTo( l.m_Position );
			}
		m_bResultChangedSinceLastSend = true;
		m_fLastSendTime = Plat_FloatTime() - 9.f;				// force send
	}

	// handle a message. returns true if the thread shuold exit
	bool HandleAMessage();

	// returns whether or not there is useful work to do
	bool AnyUsefulWorkToDo();

	// do some work, like a rendering for one light
	void DoWork();

	[[nodiscard]] Vector EstimatedUnshotAmbient() const
	{
		constexpr float sum_weights = 0.0001f;
		Vector sum_colors( sum_weights, sum_weights, sum_weights );
		// calculate an ambient color based on light calculcated so far
		if ( m_pLightList )
			for ( int i = 0; i < m_pLightList->Count(); i++ )
			{
				CLightingPreviewLightDescription& l = ( *m_pLightList )[i];
				CIncrementalLightInfo* l_info = l.m_pIncrementalInfo;
				if ( l_info && ( l_info->m_eIncrState == INCR_STATE_HAVE_FULL_RESULTS || l_info->m_eIncrState == INCR_STATE_PARTIAL_RESULTS ) )
					sum_colors += l_info->m_fTotalContribution * l.m_Color;
			}
		sum_colors.NormalizeInPlace();
		sum_colors *= 0.05f;
		return sum_colors;
	}
};


bool CIncrementalLightInfo::IsHighPriority( CLightingPreviewThread* pLPV ) const
{
	// is this lighjt prioirty-boosted in some way?
	if ( m_eIncrState == INCR_STATE_NEW && m_pLight->m_Position.WithinAABox( pLPV->m_MinViewCoords, pLPV->m_MaxViewCoords ) )
		return true; // uncalculated lights within the view range are highest priority
	return false;

}

bool CIncrementalLightInfo::IsLowerPriorityThan( CLightingPreviewThread* pLPV, CIncrementalLightInfo const& other ) const
{
	// a NEW light within the view volume is highest priority
	const bool highpriority = IsHighPriority( pLPV );
	const bool other_highpriority = other.IsHighPriority( pLPV );

	if ( highpriority && !other_highpriority )
		return false;
	if ( other_highpriority && !highpriority )
		return true;

	const int state_combo = m_eIncrState + 16 * other.m_eIncrState;
	switch ( state_combo )
	{
	case INCR_STATE_NEW + 16 * INCR_STATE_NEW:
		// if both are new, closest to eye is best
		return ( m_fDistanceToEye > other.m_fDistanceToEye );

	case INCR_STATE_NEW + 16 * INCR_STATE_NO_RESULTS:
		// new loses to something we know is probably going to contribute light
		return other.m_fTotalContribution > 0;

	case INCR_STATE_NEW + 16 * INCR_STATE_PARTIAL_RESULTS:
		return false;

	case INCR_STATE_PARTIAL_RESULTS + 16 * INCR_STATE_NEW:
		return true;

	case INCR_STATE_NO_RESULTS + 16 * INCR_STATE_NEW:
		// partial or discarded with no brightness loses to new
		return m_fTotalContribution == 0;

	case INCR_STATE_PARTIAL_RESULTS + 16 * INCR_STATE_PARTIAL_RESULTS:
		// if incrmental vs incremental, and no light from either, do most recently lit one
		if ( m_fTotalContribution == 0.0f && other.m_fTotalContribution == 0.0 && other.m_nMostRecentNonZeroContributionTimeStamp > m_nMostRecentNonZeroContributionTimeStamp )
			return true;

		// if other is black, keep this one
		if ( other.m_fTotalContribution == 0.0 && m_fTotalContribution > 0 )
			return false;
		if ( m_fTotalContribution == 0.0 && other.m_fTotalContribution > 0 )
			return true;

		// if incremental states are close, do brightest
		if ( abs( m_PartialResultsStage - other.m_PartialResultsStage ) <= 1 )
			return m_fTotalContribution < other.m_fTotalContribution;

		// else do least refined
		return m_PartialResultsStage > other.m_PartialResultsStage;

	case INCR_STATE_PARTIAL_RESULTS + 16 * INCR_STATE_NO_RESULTS:
		if ( other.m_fTotalContribution != 0.f )
			return true;
		if ( m_fTotalContribution == 0.0 && other.m_fTotalContribution == 0.0 )
			return other.m_nMostRecentNonZeroContributionTimeStamp > m_nMostRecentNonZeroContributionTimeStamp;
		return m_fTotalContribution < other.m_fTotalContribution;

	case INCR_STATE_NO_RESULTS + 16 * INCR_STATE_PARTIAL_RESULTS:
		if ( m_fTotalContribution != 0.f )
			return false;
		if ( m_fTotalContribution == 0.0 && other.m_fTotalContribution == 0.0 )
			return other.m_nMostRecentNonZeroContributionTimeStamp > m_nMostRecentNonZeroContributionTimeStamp;
		return m_fTotalContribution < other.m_fTotalContribution;

	case INCR_STATE_NO_RESULTS * 16 + INCR_STATE_NO_RESULTS:
		// if incrmental vs discarded, brightest or most recently bright wins
		if ( m_fTotalContribution == 0.0 && other.m_fTotalContribution == 0.0 )
			return other.m_nMostRecentNonZeroContributionTimeStamp > m_nMostRecentNonZeroContributionTimeStamp;
		return m_fTotalContribution < other.m_fTotalContribution;
	}
	return false;
}

void CLightingPreviewThread::InitIncrementalInformation()
{
	int calculated_bit_mask = 0;
	for ( int i = 0; i < N_INCREMENTAL_STEPS; i++ )
	{
		// bit reverse i
		int msk = 0;
		int msk_or = 1;
		int msk_test = N_INCREMENTAL_STEPS >> 1;
		while ( msk_test )
		{
			if ( i & msk_test )
				msk |= msk_or;
			msk_or <<= 1;
			msk_test >>= 1;
		}
		calculated_bit_mask |= 1<< msk;
		m_LineMask[i] = calculated_bit_mask;
	}
	// now, find which line to use when resampling a partial result
	for ( int lvl = 0; lvl < N_INCREMENTAL_STEPS; lvl++ )
	{
		for ( int linemod = 0; linemod <= N_INCREMENTAL_STEPS; linemod++ )
		{
			int closest_line = 1000000;
			for ( int chk = 0; chk <= linemod; chk++ )
				if ( m_LineMask[lvl] & ( 1 << chk ) )
					if ( abs( chk - linemod ) < abs( closest_line - linemod ) )
						closest_line = chk;
			m_ClosestLineOffset[lvl][linemod] = closest_line;
		}
	}
}

void CLightingPreviewThread::HandleGeomMessage( MessageToLPreview& msg_in )
{
	if ( m_pRtEnv )
	{
		delete m_pRtEnv;
		m_pRtEnv = nullptr;
	}
	CUtlVector<Vector>& tris = *msg_in.m_pShadowTriangleList;
	if ( !tris.IsEmpty() )
	{
		m_pRtEnv = new RayTracingEnvironment;
		for ( int i = 0; i < tris.Count(); i += 3 )
			m_pRtEnv->AddTriangle( i, tris[i], tris[1 + i], tris[2 + i], Vector( .5, .5, .5 ) );
	}
	delete msg_in.m_pShadowTriangleList;
	m_bAccStructureBuilt = false;
	DiscardResults();

}


void CLightingPreviewThread::CalculateSceneBounds()
{
	FourVectors minbound, maxbound;
	minbound.DuplicateVector( m_LastEyePosition );
	maxbound.DuplicateVector( m_LastEyePosition );
	for ( int y = 0; y < m_Positions.m_nHeight; y++ )
	{
		FourVectors const* cptr = &m_Positions.CompoundElement( 0, y );
		for ( int x = 0; x < m_Positions.m_nPaddedWidth; x++ )
		{
			minbound.x = MinSIMD( cptr->x, minbound.x );
			minbound.y = MinSIMD( cptr->y, minbound.y );
			minbound.z = MinSIMD( cptr->z, minbound.z );

			maxbound.x = MaxSIMD( cptr->x, maxbound.x );
			maxbound.y = MaxSIMD( cptr->y, maxbound.y );
			maxbound.z = MaxSIMD( cptr->z, maxbound.z );
			cptr++;
		}
	}
	m_MinViewCoords = minbound.Vec( 0 );
	m_MaxViewCoords = maxbound.Vec( 0 );
	for ( int v = 1; v < 4; v++ )
	{
		m_MinViewCoords = m_MinViewCoords.Min( minbound.Vec( v ) );
		m_MaxViewCoords = m_MaxViewCoords.Max( maxbound.Vec( v ) );
	}
}


void CLightingPreviewThread::UpdateIncrementalForNewLightList()
{
	for ( int i = 0; i < m_pLightList->Count(); i++ )
	{
		CLightingPreviewLightDescription& descr = ( *m_pLightList )[i];
		// see if we know about this light
		for ( CIncrementalLightInfo* i = m_pIncrementalLightInfoList; i; i = i->m_pNext )
		{
			if ( i->m_nObjectID == descr.m_nObjectID )
			{
				// found it!
				descr.m_pIncrementalInfo = i;
				i->m_pLight = &descr;
				break;
			}
		}

		if ( !descr.m_pIncrementalInfo )
		{
			descr.m_pIncrementalInfo = new CIncrementalLightInfo;
			descr.m_pIncrementalInfo->m_nObjectID = descr.m_nObjectID;
			descr.m_pIncrementalInfo->m_pLight = &descr;

			// add to list
			descr.m_pIncrementalInfo->m_pNext = m_pIncrementalLightInfoList;
			m_pIncrementalLightInfoList = descr.m_pIncrementalInfo;
		}
	}
}


void CLightingPreviewThread::Run()
{
	bool should_quit = false;
	while ( !should_quit )
	{
		while ( !should_quit && ( !AnyUsefulWorkToDo() || g_HammerToLPreviewMsgQueue.MessageWaiting() ) )
			should_quit |= HandleAMessage();
		if ( !should_quit && AnyUsefulWorkToDo() )
			DoWork();
		if ( m_bResultChangedSinceLastSend )
		{
			const float newtime = Plat_FloatTime();
			if ( newtime - m_fLastSendTime > 10.0f || !AnyUsefulWorkToDo() )
			{
				SendResult();
				m_bResultChangedSinceLastSend = false;
				m_fLastSendTime = newtime;
			}
		}
	}
}

bool CLightingPreviewThread::HandleAMessage()
{
	MessageToLPreview msg_in;
	g_HammerToLPreviewMsgQueue.WaitMessage( &msg_in );
	switch ( msg_in.m_MsgType )
	{
	case LPREVIEW_MSG_EXIT:
		return true;									// return from thread

	case LPREVIEW_MSG_LIGHT_DATA:
	{
		if ( m_pLightList )
			delete m_pLightList;
		m_pLightList = msg_in.m_pLightList;
		m_LastEyePosition = msg_in.m_EyePosition;
		UpdateIncrementalForNewLightList();
		DiscardResults();
	}
	break;

	case LPREVIEW_MSG_GEOM_DATA:
		HandleGeomMessage( msg_in );
		DiscardResults();
		break;

	case LPREVIEW_MSG_G_BUFFERS:
		HandleGBuffersMessage( msg_in );
		DiscardResults();
		break;
	NO_DEFAULT;
	}
	return false;
}

bool CLightingPreviewThread::AnyUsefulWorkToDo()
{
	if (  m_pLightList )
	{
		for ( int i = 0; i < m_pLightList->Count(); i++ )
		{
			CLightingPreviewLightDescription& l = ( *m_pLightList )[i];
			CIncrementalLightInfo* l_info = l.m_pIncrementalInfo;
			if ( l_info->HasWorkToDo() )
				return m_pRtEnv != nullptr;
		}
	}
	return false;
}

void CLightingPreviewThread::DoWork()
{
	if (  m_pLightList )
	{
		const CLightingPreviewLightDescription* best_l = nullptr;
		for ( int i = 0; i < m_pLightList->Count(); i++ )
		{
			const CLightingPreviewLightDescription& l = ( *m_pLightList )[i];
			const CIncrementalLightInfo* l_info = l.m_pIncrementalInfo;
			if ( l_info->HasWorkToDo() )
			{
				if ( !best_l || best_l->m_pIncrementalInfo->IsLowerPriorityThan( this, *l_info ) )
					best_l = &l;
			}
		}
		if ( best_l )
		{
			CalculateForLight( *best_l );
			if ( best_l->m_pIncrementalInfo->m_fTotalContribution != 0.f )
				m_bResultChangedSinceLastSend = true;
		}
	}
}


void CLightingPreviewThread::HandleGBuffersMessage( MessageToLPreview& msg_in )
{
	m_Albedos.CreateFromRGBA_FloatImageData( msg_in.m_pDefferedRenderingBMs[0]->Width, msg_in.m_pDefferedRenderingBMs[0]->Height, msg_in.m_pDefferedRenderingBMs[0]->RGBAData );
	m_Normals.CreateFromRGBA_FloatImageData( msg_in.m_pDefferedRenderingBMs[1]->Width, msg_in.m_pDefferedRenderingBMs[1]->Height, msg_in.m_pDefferedRenderingBMs[1]->RGBAData );
	m_Positions.CreateFromRGBA_FloatImageData( msg_in.m_pDefferedRenderingBMs[2]->Width, msg_in.m_pDefferedRenderingBMs[2]->Height, msg_in.m_pDefferedRenderingBMs[2]->RGBAData );

	m_LastEyePosition = msg_in.m_EyePosition;
	for ( int i = 0; i < ARRAYSIZE( msg_in.m_pDefferedRenderingBMs ); i++ )
		delete msg_in.m_pDefferedRenderingBMs[i];
	--n_gbufs_queued;
	m_nBitmapGenerationCounter = msg_in.m_nBitmapGenerationCounter;
	CalculateSceneBounds();
}


void CLightingPreviewThread::SendResult()
{
	m_ResultImage = m_Albedos;
 	m_ResultImage *= EstimatedUnshotAmbient();
	for ( int i = 0; i < m_pLightList->Count(); i++ )
	{
		CLightingPreviewLightDescription& l = ( *m_pLightList )[i];
		CIncrementalLightInfo* l_info = l.m_pIncrementalInfo;
		if ( l_info->m_fTotalContribution > 0.0 && l_info->m_eIncrState >= INCR_STATE_PARTIAL_RESULTS )
		{
			// need to add partials, replicated to handle undone lines
			CSIMDVectorMatrix& src = l_info->m_CalculatedContribution;
			for ( int y = 0; y < m_ResultImage.m_nHeight; y++ )
			{
				const int yo = y & ( N_INCREMENTAL_STEPS - 1 );
				const int src_y = ( y & ~( N_INCREMENTAL_STEPS - 1 ) ) + m_ClosestLineOffset[l_info->m_PartialResultsStage][yo];
				FourVectors const* cptr = &src.CompoundElement( 0, src_y );
				FourVectors* dest = &m_ResultImage.CompoundElement( 0, y );
				FourVectors const* albedo = &m_Albedos.CompoundElement( 0, y );
				for ( int x = 0; x < m_ResultImage.m_nPaddedWidth; x++ )
				{
					FourVectors albedo_value = *albedo++;
					albedo_value *= *cptr++;
					*dest++ += albedo_value;
				}
			}
		}
	}
	SendVectorMatrixAsRendering( m_ResultImage );
	m_fLastSendTime = Plat_FloatTime();
	m_bResultChangedSinceLastSend = false;
}

void CLightingPreviewThread::CalculateForLightTask( int nLineMask, int nLineMatch, const CLightingPreviewLightDescription& l, int calc_mask, float* fContributionOut )
{
	FourVectors zero_vector;
	zero_vector.x = Four_Zeros;
	zero_vector.y = Four_Zeros;
	zero_vector.z = Four_Zeros;

	FourVectors total_light = zero_vector;

	CIncrementalLightInfo* l_info = l.m_pIncrementalInfo;
	CSIMDVectorMatrix& rslt = l_info->m_CalculatedContribution;
	// figure out what lines to do
	fltx4 ThresholdBrightness = ReplicateX4( 0.1f / 1024.0f );
	FourVectors LastLinesTotalLight = zero_vector;
	int work_line_number = 0;									// for task masking
	for ( int y = 0; y < rslt.m_nHeight; y++ )
	{
		FourVectors ThisLinesTotalLight = zero_vector;
		int ybit = 1 << ( y & ( N_INCREMENTAL_STEPS - 1 ) );
		if ( ( ybit & calc_mask ) == 0 )	// do this line?
			ThisLinesTotalLight = LastLinesTotalLight;
		else
		{
			if ( ( work_line_number & nLineMask ) == nLineMatch )
			{
				for ( int x = 0; x < rslt.m_nPaddedWidth; x++ )
				{
					// shadow check
					const FourVectors& pos = m_Positions.CompoundElement( x, y );
					const FourVectors& normal = m_Normals.CompoundElement( x, y );

					FourVectors l_add = zero_vector;
					l.ComputeLightAtPoints( pos, normal, l_add, false );
					fltx4 v_or = OrSIMD( l_add.x, OrSIMD( l_add.y, l_add.z ) );
					if ( !IsAllZeros( v_or ) )
					{
						FourVectors lpos;
						lpos.DuplicateVector( l.m_Position );

						FourRays myray;
						myray.direction = lpos;
						myray.direction -= pos;
						fltx4 len = myray.direction.length();
						myray.direction *= ReciprocalSIMD( len );

						// slide towards light to avoid self-intersection
						myray.origin = myray.direction;
						myray.origin *= 0.02f;
						myray.origin += pos;

						RayTracingResult r_rslt;
						m_pRtEnv->Trace4Rays( myray, Four_Zeros, ReplicateX4( 1.0e9 ), &r_rslt );

						fltx4 mask = _mm_castsi128_ps( _mm_andnot_si128(
							_mm_cmplt_epi32( _mm_load_si128( reinterpret_cast<__m128i*>( r_rslt.HitIds ) ), _mm_setzero_si128() ),
							_mm_castps_si128( _mm_cmplt_ps( r_rslt.HitDistance, len ) ) ) );
						l_add.x = AndNotSIMD( mask, l_add.x );
						l_add.y = AndNotSIMD( mask, l_add.y );
						l_add.z = AndNotSIMD( mask, l_add.z );
						rslt.CompoundElement( x, y ) = l_add;
						l_add *= m_Albedos.CompoundElement( x, y );
						// now, supress brightness < threshold so as to not falsely think
						// far away lights are interesting
						l_add.x = AndSIMD( l_add.x, CmpGtSIMD( l_add.x, ThresholdBrightness ) );
						l_add.y = AndSIMD( l_add.y, CmpGtSIMD( l_add.y, ThresholdBrightness ) );
						l_add.z = AndSIMD( l_add.z, CmpGtSIMD( l_add.z, ThresholdBrightness ) );
						ThisLinesTotalLight += l_add;
					}
					else
						rslt.CompoundElement( x, y ) = l_add;
				}
				total_light += ThisLinesTotalLight;
			}
			work_line_number++;
		}
	}
	fltx4 lmag=total_light.length();
	*fContributionOut = lmag.m128_f32[0] + lmag.m128_f32[1] + lmag.m128_f32[2] + lmag.m128_f32[3];
}

void CLightingPreviewThread::CalculateForLight( const CLightingPreviewLightDescription& l )
{
	if ( m_pRtEnv && !m_bAccStructureBuilt )
	{
		m_bAccStructureBuilt = true;
		Msg( "Starting building acceleration structure.\n" );
		CFastTimer timer;
		timer.Start();
		m_pRtEnv->SetupAccelerationStructure();
		timer.End();
		Msg( "Acceleration structure setup done (%.2f ms)!", timer.GetDuration().GetMillisecondsF() );
	}
	CIncrementalLightInfo* l_info = l.m_pIncrementalInfo;
	Assert( l_info );
	l_info->m_CalculatedContribution.SetSize( m_Albedos.m_nWidth, m_Albedos.m_nHeight );

	// figure out which lines need to be calculated
	int prev_msk = 0;
	int new_incr_level = 0;
	if ( l_info->m_eIncrState == INCR_STATE_PARTIAL_RESULTS )
	{
		new_incr_level = 1 + l_info->m_PartialResultsStage;
		prev_msk = m_LineMask[l_info->m_PartialResultsStage];
	}
	int calc_mask=m_LineMask[new_incr_level] &~ prev_msk;

	// multihread here
	CUtlVector<float> total_light;
	BeginExecuteParallel();
	ExecuteParallel( this, &CLightingPreviewThread::CalculateForLightTask, 0b11, 0b00, l, calc_mask, &total_light[total_light.AddToTail()] );
	ExecuteParallel( this, &CLightingPreviewThread::CalculateForLightTask, 0b11, 0b01, l, calc_mask, &total_light[total_light.AddToTail()] );
	ExecuteParallel( this, &CLightingPreviewThread::CalculateForLightTask, 0b11, 0b10, l, calc_mask, &total_light[total_light.AddToTail()] );
	ExecuteParallel( this, &CLightingPreviewThread::CalculateForLightTask, 0b11, 0b11, l, calc_mask, &total_light[total_light.AddToTail()] );
	EndExecuteParallel();

	l_info->m_fTotalContribution = 0.f;
	for ( const float& light : total_light ) // accum
		l_info->m_fTotalContribution += light;

	// throw away light array if no contribution
	if ( l_info->m_fTotalContribution == 0.0 )
		l_info->m_CalculatedContribution.SetSize( 0, 0 );
	else
		l_info->m_nMostRecentNonZeroContributionTimeStamp = m_nContributionCounter;
	l_info->m_PartialResultsStage = new_incr_level;
	if ( new_incr_level == N_INCREMENTAL_STEPS-1)
		l_info->m_eIncrState = INCR_STATE_HAVE_FULL_RESULTS;
	else
		l_info->m_eIncrState = INCR_STATE_PARTIAL_RESULTS;
}

void CLightingPreviewThread::SendVectorMatrixAsRendering( CSIMDVectorMatrix const& src )
{
	Bitmap_t* ret_bm = new Bitmap_t;
	ret_bm->Init( src.m_nWidth, src.m_nHeight, IMAGE_FORMAT_RGBA8888 );
	// lets copy into the output bitmap
	for ( int y = 0; y < src.m_nHeight; y++ )
		for ( int x = 0; x < src.m_nWidth; x++ )
		{
			const Vector& color = src.Element( x, y );
			*( ret_bm->GetPixel( x, y ) + 0 ) = Min<uint8>( 255.0f, 255.0f * LinearToGammaFullRange( color.z ) );
			*( ret_bm->GetPixel( x, y ) + 1 ) = Min<uint8>( 255.0f, 255.0f * LinearToGammaFullRange( color.y ) );
			*( ret_bm->GetPixel( x, y ) + 2 ) = Min<uint8>( 255.0f, 255.0f * LinearToGammaFullRange( color.x ) );
			*( ret_bm->GetPixel( x, y ) + 3 ) = 0;
		}
	MessageFromLPreview ret_msg( LPREVIEW_MSG_DISPLAY_RESULT );
	ret_msg.m_pBitmapToDisplay = ret_bm;
	ret_msg.m_nBitmapGenerationCounter = m_nBitmapGenerationCounter;
	g_LPreviewToHammerMsgQueue.QueueMessage( ret_msg );
}




// master side of lighting preview
unsigned LightingPreviewThreadFN( void* )
{
	CLightingPreviewThread LPreviewObject;
	ThreadSetPriority( -2 );								// low
	LPreviewObject.Run();
	return 0;
}


void HandleLightingPreview()
{
	if ( GetMainWnd()->m_pLightingPreviewOutputWindow && !GetMainWnd()->m_bLightingPreviewOutputWindowShowing )
	{
		delete GetMainWnd()->m_pLightingPreviewOutputWindow;
		GetMainWnd()->m_pLightingPreviewOutputWindow = NULL;
	}

	// called during main loop
	while ( g_LPreviewToHammerMsgQueue.MessageWaiting() )
	{
		MessageFromLPreview msg;
		g_LPreviewToHammerMsgQueue.WaitMessage( &msg );
		switch( msg.m_MsgType )
		{
			case LPREVIEW_MSG_DISPLAY_RESULT:
			{
				if ( !CMapDoc::GetActiveMapDoc() || !CMapDoc::GetActiveMapDoc()->HasAnyLPreview() )
					break;
				if ( g_pLPreviewOutputBitmap )
					delete g_pLPreviewOutputBitmap;
				g_pLPreviewOutputBitmap = msg.m_pBitmapToDisplay;
				if ( g_pLPreviewOutputBitmap && g_pLPreviewOutputBitmap->Width() > 10 )
				{
					SignalUpdate( EVTYPE_BITMAP_RECEIVED_FROM_LPREVIEW );
					CLightingPreviewResultsWindow* w = GetMainWnd()->m_pLightingPreviewOutputWindow;
					if ( !GetMainWnd()->m_bLightingPreviewOutputWindowShowing )
					{
						w = new CLightingPreviewResultsWindow;
						GetMainWnd()->m_pLightingPreviewOutputWindow = w;
						w->Create( GetMainWnd() );
						GetMainWnd()->m_bLightingPreviewOutputWindowShowing = true;
					}
					if ( !w->IsWindowVisible() )
						w->ShowWindow( SW_SHOW );
					RECT existing_rect;
					w->GetClientRect( &existing_rect );
					if ( existing_rect.right != g_pLPreviewOutputBitmap->Width() - 1 || existing_rect.bottom != g_pLPreviewOutputBitmap->Height() - 1 )
					{
						CRect myRect;
						myRect.top = 0;
						myRect.left = 0;
						myRect.right = g_pLPreviewOutputBitmap->Width() - 1;
						myRect.bottom = g_pLPreviewOutputBitmap->Height() - 1;
						w->CalcWindowRect( &myRect );
						w->SetWindowPos( nullptr, 0, 0, myRect.Width(), myRect.Height(), SWP_NOMOVE | SWP_NOZORDER );
					}

					w->Invalidate( false );
					w->UpdateWindow();
				}
			break;
			}
		}
	}
}