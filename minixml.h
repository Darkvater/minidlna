/* $Id: minixml.h,v 1.7 2012/09/27 15:42:10 nanard Exp $ */
/* minimal xml parser
 * MiniDLNA media server
 * This file is part of MiniDLNA.
 *
 * The code herein is based on the MiniUPnP Project.
 * http://miniupnp.free.fr/ or http://miniupnp.tuxfamily.org/
 *
 * Copyright (c) 2006-2012, Thomas BERNARD
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * The name of the author may not be used to endorse or promote products
 *       derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */
#ifndef MINIXML_H_INCLUDED
#define MINIXML_H_INCLUDED
#define IS_WHITE_SPACE(c) ((c==' ') || (c=='\t') || (c=='\r') || (c=='\n'))

/* if a callback function pointer is set to NULL,
* the function is not called */
struct xmlparser {
	const char *xmlstart;
	const char *xmlend;
	const char *xml;	/* pointer to current character */
	int xmlsize;
	void * data;
	void(*starteltfunc) (void *, const char *, int);
	void(*endeltfunc) (void *, const char *, int);
	void(*datafunc) (void *, const char *, int);
	void(*attfunc) (void *, const char *, int, const char *, int);
};

/* parsexml()
* the xmlparser structure must be initialized before the call
* the following structure members have to be initialized :
* xmlstart, xmlsize, data, *func
* xml is for internal usage, xmlend is computed automatically */
void parsexml(struct xmlparser *);

#endif
