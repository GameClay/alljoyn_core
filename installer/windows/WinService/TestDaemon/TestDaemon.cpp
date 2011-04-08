/**
 * @file
 * TestDaemon.cpp
 */

/******************************************************************************
 * Copyright 2010-2011, Qualcomm Innovation Center, Inc.
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
 ******************************************************************************/

#include "..\WinService\DaemonLib.h"

int _tmain(int argc, char * argv[])
{
	printf("AllJoyn Daemon Windows Service Version xx \n");
	printf("arg c=%d\n",argc);
	for(int i=0; i < argc ; i++)
		printf("%s \n", argv[i]); 
	LoadDaemon(argc, argv);
	getchar();
	return 0;
}

