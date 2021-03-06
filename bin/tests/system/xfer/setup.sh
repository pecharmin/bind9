#!/bin/sh
#
# Copyright (C) 2001, 2002, 2004, 2007, 2011-2016  Internet Systems Consortium, Inc. ("ISC")
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.

SYSTEMTESTTOP=..
. $SYSTEMTESTTOP/conf.sh

$SHELL clean.sh

$SHELL ../genzone.sh 1 6 7 >ns1/slave.db
$SHELL ../genzone.sh 1 6 7 >ns1/edns-expire.db
$SHELL ../genzone.sh 2 3 >ns2/example.db
$SHELL ../genzone.sh 2 3 >ns2/tsigzone.db
$SHELL ../genzone.sh 6 3 >ns6/master.db
$SHELL ../genzone.sh 7 >ns7/master2.db

rm -f ns4/*.db ns4/*.jnl
cp -f ns4/root.db.in ns4/root.db
$PERL -e 'for ($i=0;$i<10000;$i++){ printf("x%u 0 in a 10.53.0.1\n", $i);}' >> ns4/root.db
cp -f ns4/named.conf.base ns4/named.conf

cp ns2/slave.db.in ns2/slave.db
touch -t 200101010000 ns2/slave.db

cp ns2/mapped.db.in ns2/mapped.db

$PERL -e 'for ($i=0;$i<4096;$i++){ printf("name%u 259200 A 1.2.3.4\nname%u 259200 TXT \"Hello World %u\"\n", $i, $i, $i);}' > ns8/small.db
$PERL -e 'printf("large IN TYPE45234 \\# 48000 "); for ($i=0;$i<16*3000;$i++) { printf("%02x", $i % 256); } printf("\n");' > ns8/large.db
