#pragma once

#include <qpdf/QPDF.hh>

void removeUnusedFonts(QPDF &qpdf);
void subsetFonts(QPDF &qpdf);
