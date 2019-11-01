//========= Copyright � 1996-2005, Valve Corporation, All rights reserved. ====
//
// Purpose:
//
//=============================================================================

#include "stdafx.h"
#include "hammer.h"
#include "lprvwindow.h"
#include "gameconfig.h"
#include "lpreview_thread.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"



BEGIN_MESSAGE_MAP( CLightingPreviewResultsWindow, CWnd )
	//{{AFX_MSG_MAP(CTextureWindow)
	ON_WM_PAINT()
	//}}AFX_MSG_MAP
END_MESSAGE_MAP()


//-----------------------------------------------------------------------------
// Purpose: Constructor. Initializes data members.
//-----------------------------------------------------------------------------
CLightingPreviewResultsWindow::CLightingPreviewResultsWindow()
{
}

//-----------------------------------------------------------------------------
// Purpose: Destructor.
//-----------------------------------------------------------------------------
CLightingPreviewResultsWindow::~CLightingPreviewResultsWindow()
{
}

//-----------------------------------------------------------------------------
// Purpose:
// Input  : *pParentWnd -
//			rect -
//-----------------------------------------------------------------------------
void CLightingPreviewResultsWindow::Create( CWnd* pParentWnd )
{
	static CString LPreviewWndClassName;

	if ( LPreviewWndClassName.IsEmpty() )
		// create class
		LPreviewWndClassName = AfxRegisterWndClass( CS_DBLCLKS | CS_HREDRAW | CS_VREDRAW, LoadCursor( nullptr, IDC_ARROW ), static_cast<HBRUSH>( GetStockObject( BLACK_BRUSH ) ), nullptr );

	RECT rect;
	rect.left = 500; rect.right = 600;
	rect.top = 500; rect.bottom = 600;

	CWnd::CreateEx( 0, LPreviewWndClassName, "LightingPreviewWindow", WS_OVERLAPPEDWINDOW | WS_SIZEBOX, rect, pParentWnd, 0, nullptr );
}

//-----------------------------------------------------------------------------
// Purpose:
//-----------------------------------------------------------------------------
void CLightingPreviewResultsWindow::OnPaint()
{
	CPaintDC dc( this ); // device context for painting

	CRect clientrect;
	GetClientRect( clientrect );
	if ( g_pLPreviewOutputBitmap )
	{
		// blit it
		BITMAPINFOHEADER mybmh;
		memset( &mybmh, 0, sizeof( BITMAPINFOHEADER ) );
		mybmh.biSize = sizeof( BITMAPINFOHEADER );
		mybmh.biWidth = g_pLPreviewOutputBitmap->Width();
		mybmh.biHeight = -g_pLPreviewOutputBitmap->Height();
		mybmh.biPlanes = 1;
		mybmh.biBitCount = 32;
		mybmh.biCompression = BI_RGB;
		mybmh.biSizeImage = g_pLPreviewOutputBitmap->Width() * g_pLPreviewOutputBitmap->Height();

		StretchDIBits(
			dc.GetSafeHdc(), clientrect.left, clientrect.top, 1 + ( clientrect.right - clientrect.left ),
			1 + ( clientrect.bottom - clientrect.top ), 0, 0, g_pLPreviewOutputBitmap->Width(), g_pLPreviewOutputBitmap->Height(),
			g_pLPreviewOutputBitmap->GetBits(), (BITMAPINFO*)&mybmh, DIB_RGB_COLORS, SRCCOPY );
	}
}