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
 * Contains reply for well-know name query.
 */
public class IsAtAnswer implements Serializable {
	private String mGuid;
	private String mNames;
	private String mUUID;
	
	public IsAtAnswer(String guid, String wkn, String uuid){
		mGuid = guid;
		mNames = wkn;
		mUUID = uuid;
	}
	
	public String getGuid(){
		return mGuid;
	}
	
	public void setGuid(String guid){
		mGuid = guid;
	}
	
	public String getNames(){
		return mNames;
	}
	
	public void setNames(String wkn){
		mNames = wkn;
	}
	
	public String getServiceUUID(){
		return mUUID;
	}
	
	public void setServiceUUID(String uuid){
		mUUID = uuid;
	}
}

