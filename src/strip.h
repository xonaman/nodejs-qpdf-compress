#pragma once

#include <qpdf/QPDF.hh>

void stripMetadata(QPDF &qpdf);
void stripIccProfiles(QPDF &qpdf);
void stripEmbeddedFiles(QPDF &qpdf);
void stripJavaScript(QPDF &qpdf);
void stripDocumentOverhead(QPDF &qpdf);
void removeUnusedResources(QPDF &qpdf);
