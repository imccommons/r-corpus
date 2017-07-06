/*
 * Copyright 2017 Patrick O. Perry.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <R_ext/Riconv.h>

#include "corpus/src/array.h"
#include "corpus/src/unicode.h"
#include "rcorpus.h"

static const char *translate(SEXP charsxp)
{
	const char *ptr = CHAR(charsxp);
	size_t n = (size_t)XLENGTH(charsxp);
	char *buf, *out;
	size_t nbuf, status;
	void *cd;

	if (n == 0) {
		return ptr;
	}

	cd = Riconv_open("", "UTF-8");
	nbuf = n;
	buf = R_alloc(nbuf + 1, 1);

again:
	out = buf;
	status = Riconv(cd, &ptr, &n, &out, &nbuf);
	if (status == (size_t)-1) {
		switch (errno) {
		case EILSEQ: // invalid multibyte sequence (can't happen)
		case EINVAL: // incomplete multibyte sequence (can't happen)
			error("invalid UTF-8 byte sequence");
			break;

		case E2BIG: // no room for the next converted character
			nbuf *= 2;
			buf = R_alloc(nbuf + 1, 1);
			ptr = CHAR(charsxp);
			goto again;
		default:
			error("unrecognized iconv errno value");
		}
	}
	*out = '\0';

	Riconv_close(cd);
	return buf;
}


#define NEEDS(n) \
	do { \
		if ((n) && (nbuf > nbuf_max - (n))) { \
			nbuf_max0 = nbuf_max; \
			corpus_array_size_add(&nbuf_max, 1, nbuf, (n)); \
			buf = S_realloc(buf, nbuf_max, nbuf_max0, 1); \
		} \
	} while (0)

#define PRINT_SPACES(n) \
	do { \
		NEEDS(n); \
		if ((n) > 0) { \
			memset(buf + nbuf, ' ', (n)); \
			nbuf += (n); \
		} \
	} while (0)

#define PRINT_CHAR(ch) \
	do { \
		NEEDS(1); \
		buf[nbuf] = (ch); \
		nbuf++; \
	} while (0)

#define PRINT_STRING(str, n) \
	do { \
		NEEDS(n); \
		memcpy(buf + nbuf, str, (n)); \
		nbuf += (n); \
	} while (0)

#define PRINT_ENTRY(str, n, pad) \
	do { \
		if (right) PRINT_SPACES(pad); \
		PRINT_STRING(str, n); \
		if (!right) PRINT_SPACES(pad); \
	} while (0)

#define FLUSH() \
	do { \
		PRINT_CHAR('\0'); \
		Rprintf("%s", buf); \
		nbuf = 0; \
	} while (0)

static int print_range(SEXP sx, int begin, int end, int print_gap,
			int right, int max, int namewidth,
			const int *colwidths)
{
	SEXP elt, name, dim_names, row_names, col_names;
	R_xlen_t ix;
	const char *str;
	char *buf;
	int nbuf, nbuf_max, nbuf_max0;
	int i, j, nrow, n, w, nprint, width, utf8;

	dim_names = getAttrib(sx, R_DimNamesSymbol);
	row_names = VECTOR_ELT(dim_names, 0);
	col_names = VECTOR_ELT(dim_names, 1);
	nrow = nrows(sx);
	nprint = 0;
	utf8 = 1;

	nbuf = 0;
	nbuf_max = 128;
	buf = R_alloc(nbuf_max + 1, 1);

	if (col_names != R_NilValue) {
		PRINT_SPACES(namewidth);

		for (j = begin; j < end; j++) {
			name = STRING_ELT(col_names, j);
			if (name == NA_STRING) {
				str = "NA";
				w = 2;
				n = 2;
			} else {
				str = translate(name);
				w = charsxp_width(name, utf8);
				n = strlen(str);
			}
			if (j > begin || row_names != R_NilValue) {
				PRINT_SPACES(print_gap);
			}
			PRINT_ENTRY(str, n, colwidths[j] - w);
		}
		PRINT_CHAR('\n');
		FLUSH();
	}

	for (i = 0; i < nrow; i++) {
		if (nprint == max) {
			FLUSH();
			return nprint;
		}

		if (row_names != R_NilValue) {
			name = STRING_ELT(row_names, i);
			if (name == NA_STRING) {
				str = "NA";
				w = 2;
				n = 2;
			} else {
				str = translate(name);
				w = charsxp_width(name, utf8);
				n = strlen(str);
			}

			PRINT_STRING(str, n);
			PRINT_SPACES(namewidth - w);
		}

		for (j = begin; j < end; j++) {
			if (nprint == max) {
				PRINT_CHAR('\n');
				FLUSH();
				return nprint;
			}
			nprint++;

			width = colwidths[j];
			ix = (R_xlen_t)i + (R_xlen_t)j * (R_xlen_t)nrow;
			elt = STRING_ELT(sx, ix);

			if (j > begin || row_names != R_NilValue) {
				PRINT_SPACES(print_gap);
			}

			str = translate(elt);
			w = charsxp_width(elt, utf8);
			n = strlen(str);
			PRINT_ENTRY(str, n, width - w);
		}

		PRINT_CHAR('\n');
		FLUSH();

		if ((i + i) % RCORPUS_CHECK_INTERRUPT == 0) {
			R_CheckUserInterrupt();
		}
	}

	return nprint;
}


SEXP print_table(SEXP sx, SEXP sprint_gap, SEXP sright, SEXP smax,
		SEXP swidth)
{
	SEXP elt, dim_names, row_names, col_names;
	R_xlen_t ix, nx;
	int i, j, nrow, ncol;
	int print_gap, right, max, width, utf8;
	int begin, end, w, nprint, linewidth, namewidth, *colwidths;

	dim_names = getAttrib(sx, R_DimNamesSymbol);
	row_names = VECTOR_ELT(dim_names, 0);
	col_names = VECTOR_ELT(dim_names, 1);
	nrow = nrows(sx);
	ncol = ncols(sx);
	nx = XLENGTH(sx);
	utf8 = 1;

	print_gap = INTEGER(sprint_gap)[0];
	right = LOGICAL(sright)[0] == TRUE;
	width = INTEGER(swidth)[0];
	max = INTEGER(smax)[0];

	namewidth = 0;
	if (row_names == R_NilValue) {
		namewidth = 0;
	} else {
		for (i = 0; i < nrow; i++) {
			elt = STRING_ELT(row_names, i);
			w = (elt == NA_STRING) ? 2 : charsxp_width(elt, utf8);
			if (w > namewidth) {
				namewidth = w;
			}
		}
	}

	colwidths = (void *)R_alloc(ncol, sizeof(*colwidths));
	memset(colwidths, 0, ncol * sizeof(*colwidths));
	if (col_names != R_NilValue) {
		for (j = 0; j < ncol; j++) {
			elt = STRING_ELT(col_names, j);
			if (elt == NA_STRING) {
				colwidths[j] = 2;
			} else {
				colwidths[j] = charsxp_width(elt, utf8);
			}
		}
	}

	j = 0;
	for (ix = 0; ix < nx; ix++) {
		elt = STRING_ELT(sx, ix);
		if (elt == NA_STRING) {
			w = 0;
		} else {
			w = charsxp_width(elt, utf8);
		}

		if (w > colwidths[j]) {
			colwidths[j] = w;
		}

		if ((ix + 1) % nrow == 0) {
			j++;
		}
	}

	nprint = 0;
	begin = 0;
	while (begin != ncol) {
		linewidth = namewidth;
		end = begin;

		while (end != ncol) {
			// break if including the column puts us over the
			// width; we do the calculations like this to
			// avoid integer overflow

			if (end > begin || row_names != R_NilValue) {
				if (linewidth >= width - print_gap) {
					break;
				}
				linewidth += print_gap;
			}

			if (linewidth >= width - colwidths[end]) {
				break;
			}
			linewidth += colwidths[end];

			end++;
		}

		if (begin == end) {
			// include at least one column, even if it
			// puts us over the width
			end++;
		}

		nprint += print_range(sx, begin, end, print_gap, right,
				      max - nprint, namewidth, colwidths);
		begin = end;
	}

	if (ncol == 0) {
		nprint += print_range(sx, 0, 0, print_gap, right,
				      max - nprint, namewidth, colwidths);
	}

	return ScalarInteger(nprint);
}
