#!/bin/sh
#
# Copyright (C) 2000, 2001, 2004, 2007, 2009-2012, 2014-2016  Internet Systems Consortium, Inc. ("ISC")
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.

#
# Clean up after zone transfer tests.
#

rm -f */named.memstats
rm -f */named.run
rm -f Kxxx.*
rm -f dig.out.*
rm -f jp.out.ns3.*
rm -f ns*/named.lock
rm -f ns1/*.jnl ns2/*.jnl ns3/*.jnl
rm -f ns1/example.db ns1/unixtime.db ns1/yyyymmddvv.db ns1/update.db ns1/other.db ns1/keytests.db
rm -f ns1/many.test.db
rm -f ns1/md5.key ns1/sha1.key ns1/sha224.key ns1/sha256.key ns1/sha384.key
rm -f ns1/sha512.key ns1/ddns.key
rm -f ns2/example.bk
rm -f ns2/update.bk ns2/update.alt.bk
rm -f ns3/*.signed
rm -f ns3/K*
rm -f ns3/delegation.test.db
rm -f ns3/dnskey.test.db
rm -f ns3/dsset-*
rm -f ns3/example.db
rm -f ns3/many.test.bk
rm -f ns3/nsec3param.test.db
rm -f nsupdate.out*
rm -f typelist.out.*
