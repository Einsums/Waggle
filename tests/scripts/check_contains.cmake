#----------------------------------------------------------------------------------------------
# Copyright (c) The Einsums Developers. All rights reserved.
# Licensed under the MIT License. See LICENSE.txt in the project root for license information.
#----------------------------------------------------------------------------------------------

# Fail unless FILE exists and contains EXPECT. Run with cmake -DFILE=... -DEXPECT=... -P.
if(NOT EXISTS "${FILE}")
  message(FATAL_ERROR "${FILE} was not written")
endif()
file(READ "${FILE}" _contents)
string(FIND "${_contents}" "${EXPECT}" _at)
if(_at EQUAL -1)
  message(FATAL_ERROR "${FILE} does not contain \"${EXPECT}\"")
endif()
