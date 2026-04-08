#pragma once

#include <qpdf/QPDF.hh>

void coalesceContentStreams(QPDF &qpdf);
void minifyContentStreams(QPDF &qpdf);
