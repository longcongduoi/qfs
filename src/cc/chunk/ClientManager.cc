//---------------------------------------------------------- -*- Mode: C++ -*-
// $Id$
//
// Created 2006/03/28
// Author: Sriram Rao
//
// Copyright 2008-2012 Quantcast Corp.
// Copyright 2006-2008 Kosmix Corp.
//
// This file is part of Kosmos File System (KFS).
//
// Licensed under the Apache License, Version 2.0
// (the "License"); you may not use this file except in compliance with
// the License. You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
// implied. See the License for the specific language governing
// permissions and limitations under the License.
//
// 
//----------------------------------------------------------------------------

#include "ClientManager.h"

namespace KFS
{

ClientManager gClientManager;

bool 
ClientManager::BindAcceptor(int port)
{
    const bool kBindOnlyFlag = true;
    mAcceptor = new Acceptor(port, this, kBindOnlyFlag);
    return mAcceptor->IsAcceptorStarted();
}

bool 
ClientManager::StartListening()
{
    if (! mAcceptor) {
        return false;
    }
    mAcceptor->StartListening();
    return mAcceptor->IsAcceptorStarted();
}

}
