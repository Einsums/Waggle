#----------------------------------------------------------------------------------------------
# Copyright (c) The Einsums Developers. All rights reserved.
# Licensed under the MIT License. See LICENSE.txt in the project root for license information.
#----------------------------------------------------------------------------------------------

# Fail if FILE exists. Run with cmake -DFILE=... -P.
if(EXISTS "${FILE}")
  message(FATAL_ERROR "${FILE} was written")
endif()
