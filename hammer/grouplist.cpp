//========= Copyright � 1996-2005, Valve Corporation, All rights reserved. ============//
//
// Purpose: Implements a list view for visgroups. Supports drag and drop, and
//			posts a registered windows message to the list view's parent window
//			when visgroups are hidden or shown.
//
//=============================================================================//

#include "stdafx.h"
#include "GroupList.h"
#include "VisGroup.h"

// memdbgon must be the last include file in a .cpp file!!!
#include <tier0/memdbgon.h>


//-----------------------------------------------------------------------------
// Purpose: 
// Input  : nFlags - 
//			point - 
//-----------------------------------------------------------------------------
CGroupList::CGroupList()
{
}


//-----------------------------------------------------------------------------
// Purpose: 
// Input  : nFlags - 
//			point - 
//-----------------------------------------------------------------------------
CGroupList::~CGroupList()
{
}


//-----------------------------------------------------------------------------
// Purpose: 
// Input  : eDropType - 
//			nFlags - 
//			point - 
//-----------------------------------------------------------------------------
void CGroupList::OnRenameItem(void *pItem, const char *pszText) 
{
	Assert(pItem);
	Assert(pszText);

	if (!pItem || !pszText)
		return;

	CVisGroup *pVisGroup = (CVisGroup *)pItem;
	pVisGroup->SetName(pszText);
}


//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
void CGroupList::AddVisGroup(CVisGroup *pVisGroup)
{
	AddItem(pVisGroup, pVisGroup->GetParent(), pVisGroup->GetName(), true);
	for (int i = 0; i < pVisGroup->GetChildCount(); i++)
	{
		AddVisGroup(pVisGroup->GetChild(i));
	}
}


//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
void CGroupList::UpdateVisGroup(CVisGroup *pVisGroup)
{
	UpdateItem(pVisGroup, pVisGroup->GetName());
}
