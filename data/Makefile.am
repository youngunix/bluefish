
SUBDIRS = bflang bflib templates

EXTRA_DIST = $(nobase_bfdata_DATA) $(bfdata_SCRIPTS)

MAINTAINERCLEANFILES = Makefile.in

bfdatadir = $(pkgdatadir)
nobase_bfdata_DATA = \
	default_accelmap \
	encodings \
	snippets \
	jsbeautifier/__init__.py \
	jsbeautifier/__version__.py \
	jsbeautifier/unpackers/packer.py \
	jsbeautifier/unpackers/javascriptobfuscator.py \
	jsbeautifier/unpackers/myobfuscate.py \
	jsbeautifier/unpackers/__init__.py \
	jsbeautifier/unpackers/evalbased.py \
	jsbeautifier/unpackers/urlencode.py \
	colorprofiles/Light_theme \
	colorprofiles/Dark_theme

bfdata_SCRIPTS = \
	lorem-ipsum-generator \
	jsbeautify \
	jsmin.py \
	json_prettyprint.py \
	css_decompressor \
	cssmin.py
