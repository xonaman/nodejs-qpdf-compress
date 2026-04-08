#pragma once

#include <qpdf/QPDF.hh>

void deduplicateStreams(QPDF &qpdf);
void flattenForms(QPDF &qpdf);
void flattenPageTree(QPDF &qpdf);
