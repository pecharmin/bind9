/*
 * Copyright (C) 2000, 2001, 2004, 2005, 2007, 2011, 2012, 2016  Internet Systems Consortium, Inc. ("ISC")
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

/* $Id$ */

#ifndef UNIX_ERRNO2RESULT_H
#define UNIX_ERRNO2RESULT_H 1

/*! \file */

/* XXXDCL this should be moved to lib/isc/include/isc/errno2result.h. */

#include <errno.h>		/* Provides errno. */

#include <isc/lang.h>
#include <isc/types.h>

ISC_LANG_BEGINDECLS

#define isc__errno2result(x) isc___errno2result(x, __FILE__, __LINE__)

isc_result_t
isc___errno2result(int posixerrno, const char *file, unsigned int line);

ISC_LANG_ENDDECLS

#endif /* UNIX_ERRNO2RESULT_H */
