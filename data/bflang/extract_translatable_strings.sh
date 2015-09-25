#!/bin/sh

xmlstarlet sel -t -n -v '//bflang/header/option/@description' *.bflang2 2>/dev/null|sort|uniq|sed -e 's/.*/N_("&")/' ../../src/xmlstrings.h
