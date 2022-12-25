#!/bin/sh
set -e

aclocal
autoheader
automake --include-deps --add-missing --copy
autoconf
