/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 *
 * The contents of this file are subject to the Netscape Public License
 * Version 1.0 (the "NPL"); you may not use this file except in
 * compliance with the NPL.  You may obtain a copy of the NPL at
 * http://www.mozilla.org/NPL/
 *
 * Software distributed under the NPL is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the NPL
 * for the specific language governing rights and limitations under the
 * NPL.
 *
 * The Initial Developer of this code under the NPL is Netscape
 * Communications Corporation.  Portions created by Netscape are
 * Copyright (C) 1998 Netscape Communications Corporation.  All Rights
 * Reserved.
 */

//////////////////////////////////////////////////////////////////////////
//                                                                      //
// Name:        RDFUtils.h                                              //
//                                                                      //
// Description:	Misc RDF XFE specific utilities.                        //
//                                                                      //
// Author:		Ramiro Estrugo <ramiro@netscape.com>                    //
//                                                                      //
//////////////////////////////////////////////////////////////////////////

#ifndef _xfe_rdf_utils_h_
#define _xfe_rdf_utils_h_

#include "htrdf.h"
#include "xp_core.h"
#include "Command.h"

class XFE_RDFUtils
{
public:
    
 	XFE_RDFUtils() {}
 	~XFE_RDFUtils() {}

	//////////////////////////////////////////////////////////////////////
	//                                                                  //
	// XFE Command utilities                                            //
	//                                                                  //
	// Is the URL a 'special' command url that translates to an FE      //
	// command ?                                                        //
	//                                                                  //
	//////////////////////////////////////////////////////////////////////
    static XP_Bool        ht_IsFECommand        (HT_Resource  item);
    static CommandType    ht_GetFECommand       (HT_Resource  item);
};

#endif // _xfe_rdf_utils_h_
