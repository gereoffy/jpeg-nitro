/* libxml2's xmlversion.h for nitroview's build of it: the XML reader only (what libvisio uses).
   No output, XPath, schemas, HTML, catalogs, iconv, zlib, modules or legacy APIs. */
#ifndef __XML_VERSION_H__
#define __XML_VERSION_H__
#define LIBXML_DOTTED_VERSION "2.13.8"
#define LIBXML_VERSION 21308
#define LIBXML_VERSION_STRING "21308"
#define LIBXML_VERSION_EXTRA ""
#define LIBXML_TEST_VERSION xmlCheckVersion(21308);
#define LIBXML_THREAD_ENABLED
#define LIBXML_TREE_ENABLED
#define LIBXML_PUSH_ENABLED
#define LIBXML_READER_ENABLED
#define LIBXML_ISO8859X_ENABLED
#include <libxml/xmlexports.h>
#endif
