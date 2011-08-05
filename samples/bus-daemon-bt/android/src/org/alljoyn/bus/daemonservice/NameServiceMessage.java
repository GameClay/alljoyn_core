/*
 * Copyright 2010 - 2011, Qualcomm Innovation Center, Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *        http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 *
 */
package org.alljoyn.bus.daemonservice;

import java.io.Serializable;

/**
 * Request and reply for well-know name advertisement and discovery.
 */
public class NameServiceMessage implements Serializable {
	public final static int TYPE_ADVERTISE_NAME_REQUEST= 0;
	public final static int TYPE_DISCOVER_NAME_REQUEST = 1;
	public final static int TYPE_DISCOVER_NAME_REPLEY = 2;
	
	private int mType = -1;
	private String mNamePrefix = null;
	private String mGuid = null;
	private String mWkn = null;
	private String mUUID = null;
	
	public NameServiceMessage(int type){
		mType = type;
	}
	
	public int getType(){
		return mType;
	}

	public String getNamePrefix(){
		return mNamePrefix;
	}
		
	public String getGuid(){
		return mGuid;
	}
	
	public String getWkn(){
		return mWkn;
	}

	public String getServiceUUID(){
		return mUUID;
	}
	
	public boolean fillAdvertiseRequest(String guid, String uuid, String wkn){
		if(mType != TYPE_ADVERTISE_NAME_REQUEST) return false;
		mGuid = guid;
		mUUID = uuid;
		mWkn = wkn;
		return true;
	}

	public boolean fillDiscoverRequest(String prefix){
		if(mType != TYPE_DISCOVER_NAME_REQUEST) return false;
		mNamePrefix = prefix;
		return true;
	}
	
	public boolean fillDiscoverReply(String guid, String uuid, String wkn){
		if(mType != TYPE_DISCOVER_NAME_REPLEY) return false;
		mGuid = guid;
		mUUID = uuid;
		mWkn = wkn;
		return true;
	}
}

