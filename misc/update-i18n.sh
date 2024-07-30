#!/bin/sh

set -eu

LANGUAGES="ru en_GB en_US cs_CZ"

LUPDATE="/usr/lib/qt6/bin/lupdate"
LRELEASE="/usr/lib/qt6/bin/lrelease"

for LANG in $LANGUAGES; do
    $LUPDATE \
	svcore/*/*.h svcore/*/*.cpp \
	svcore/*/*/*.h svcore/*/*/*.cpp \
	svgui/*/*.h svgui/*/*.cpp \
	svapp/*/*.h svapp/*/*.cpp \
	*/*.h */*.cpp \
	-ts i18n/sonic-visualiser_$LANG.ts
done

for LANG in $LANGUAGES; do
    $LRELEASE i18n/sonic-visualiser_$LANG.ts
done

