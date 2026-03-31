#pragma once

#include <qpdf/QPDF.hh>

void stripMetadata(QPDF &qpdf);
void removeUnusedFonts(QPDF &qpdf);
void coalesceContentStreams(QPDF &qpdf);
void deduplicateStreams(QPDF &qpdf);
void subsetFonts(QPDF &qpdf);
void stripIccProfiles(QPDF &qpdf);
void stripEmbeddedFiles(QPDF &qpdf);
void stripJavaScript(QPDF &qpdf);
void flattenForms(QPDF &qpdf);
void flattenPageTree(QPDF &qpdf);
void minifyContentStreams(QPDF &qpdf);
