/*
	sim_vcd_file.c

	Implements a Value Change Dump file outout to generate
	traces & curves and display them in gtkwave.

	Copyright 2008, 2009 Michel Pollet <buserror@gmail.com>

 	This file is part of simavr.

	simavr is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	simavr is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with simavr.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>
#include <ctype.h>
#include "sim_vcd_file.h"
#include "sim_avr.h"
#include "sim_time.h"

static void
_avr_vcd_notify(
		struct avr_irq_t * irq,
		uint32_t value,
		void * param);

int
avr_vcd_init(
		struct avr_t * avr,
		const char * filename,
		avr_vcd_t * vcd,
		uint32_t period)
{
	memset(vcd, 0, sizeof(avr_vcd_t));
	vcd->avr = avr;
	vcd->filename = strdup(filename);
	vcd->period = avr_usec_to_cycles(vcd->avr, period);

	return 0;
}
#if 0
static const char *
strrstr(
		const char *haystack,
		const char *needle )
{
	int nl = strlen(needle);
	int hl = strlen(haystack);

	if (hl < nl)
		return NULL;
	if (hl == nl)
		return strcmp(haystack, needle) ? NULL : haystack;

	const char *d = haystack + hl - nl;
	while (d >= haystack) {
		for (int i = 0; i <= nl; i++)
			if (needle[i] == 0)
				return d;	// found it!
			else if (needle[i] != d[i])	// no match
				break;
		d--;
	}
	return NULL;
}
#endif

typedef struct argv_t {
	uint32_t size, argc;
	char * line;
	char * argv[];
} argv_t, *argv_p;

argv_p
argv_realloc(
	argv_p	argv,
	uint32_t size )
{
	argv = realloc(argv, sizeof(argv_t) + (size * sizeof(argv->argv[0])));
	argv->size = size;
	return argv;
}

argv_p
argv_parse(
	argv_p	argv,
	char * line )
{
	if (!argv)
		argv = argv_realloc(argv, 8);
	argv->argc = 0;

	/* strip end of lines and trailing spaces */
	char *d = line + strlen(line);
	while ((d - line) > 0 && *(--d) <= ' ')
		*d = 0;
	/* stop spaces + tabs */
	char *s = line;
	while (*s && *s <= ' ')
		s++;
	argv->line = s;
	char * a = NULL;
	do {
		if (argv->argc == argv->size)
			argv = argv_realloc(argv, argv->size + 8);
		if ((a = strsep(&s, " \t")) != NULL)
			argv->argv[argv->argc++] = a;
	} while (a);
	argv->argv[argv->argc] = NULL;
	return argv;
}



int
avr_vcd_init_input(
		struct avr_t * avr,
		const char * filename, 	// filename to read
		avr_vcd_t * vcd )		// vcd struct to initialize
{
	memset(vcd, 0, sizeof(avr_vcd_t));
	vcd->avr = avr;
	vcd->filename = strdup(filename);

	vcd->input = fopen(vcd->filename, "r");
	if (!vcd->input) {
		perror(filename);
		return -1;
	}
	char line[1024];
	argv_p v = NULL;

	while (fgets(line, sizeof(line), vcd->input)) {
		if (!line[0])	// technically can't happen, but make sure next line works
			continue;
		v = argv_parse(v, line);
		// ignore multiline stuff
		if (v->line[0] != '$')
			continue;

		const char * end = !strcmp(v->argv[v->argc - 1], "$end") ?
								v->argv[v->argc - 1] : NULL;
		const char *keyword = v->argv[0];

		if (keyword == end)
			keyword = NULL;
		if (!keyword)
			continue;

		printf("keyword '%s' end '%s'\n", keyword, end);
		if (!strcmp(keyword, "$enddefinitions"))
			break;
		if (!strcmp(keyword, "$timescale")) {
			double cnt = 0;
			char *si = v->argv[1];
			while (si && *si && isdigit(*si))
				cnt = (cnt * 10) + (*si++ - '0');
			while (*si == ' ')
				si++;
			if (!strcmp(si, "ns"))
				cnt /= 1000;
			printf("cnt %d; unit %s\n", (int)cnt, si);
		} else if (!strcmp(keyword, "$var")) {
			const char *name = v->argv[4];

			vcd->signal[vcd->signal_count].alias = v->argv[3][0];
			vcd->signal[vcd->signal_count].size = atoi(v->argv[2]);
			strncpy(vcd->signal[vcd->signal_count].name, name,
						sizeof(vcd->signal[0].name));

			vcd->signal_count++;
		}
	}
	free(v);
	return 0;
}

void
avr_vcd_close(
		avr_vcd_t * vcd)
{
	avr_vcd_stop(vcd);

	/* dispose of any link and hooks */
	for (int i = 0; i < vcd->signal_count; i++) {
		avr_vcd_signal_t * s = &vcd->signal[i];

		avr_free_irq(&s->irq, 1);
	}

	if (vcd->filename) {
		free(vcd->filename);
		vcd->filename = NULL;
	}
}

static void
_avr_vcd_notify(
		struct avr_irq_t * irq,
		uint32_t value,
		void * param)
{
	avr_vcd_t * vcd = (avr_vcd_t *)param;

	if (!vcd->output)
		return;

	/*
	 * buffer starts empty, the first trace will resize it to AVR_VCD_LOG_CHUNK_SIZE,
	 * further growth will resize it accordingly.
	 */
	if (vcd->logindex >= vcd->logsize) {
		vcd->logsize += AVR_VCD_LOG_CHUNK_SIZE;
		vcd->log = (avr_vcd_log_p)realloc(vcd->log, vcd->logsize * sizeof(vcd->log[0]));
		AVR_LOG(vcd->avr, LOG_TRACE, "%s trace buffer resized to %d\n",
				__func__, (int)vcd->logsize);
		if ((vcd->logsize / AVR_VCD_LOG_CHUNK_SIZE) == 8) {
			AVR_LOG(vcd->avr, LOG_WARNING, "%s log size runnaway (%d) flush problem?\n",
					__func__, (int)vcd->logsize);
		}
		if (!vcd->log) {
			AVR_LOG(vcd->avr, LOG_ERROR, "%s log resizing, out of memory (%d)!\n",
					__func__, (int)vcd->logsize);
			vcd->logsize = 0;
			return;
		}
	}
	avr_vcd_signal_t * s = (avr_vcd_signal_t*)irq;
	avr_vcd_log_t *l = &vcd->log[vcd->logindex++];
	l->signal = s;
	l->when = vcd->avr->cycle;
	l->value = value;
}


static char *
_avr_vcd_get_float_signal_text(
		avr_vcd_signal_t * s,
		char * out)
{
	char * dst = out;

	if (s->size > 1)
		*dst++ = 'b';

	for (int i = s->size; i > 0; i--)
		*dst++ = 'x';
	if (s->size > 1)
		*dst++ = ' ';
	*dst++ = s->alias;
	*dst = 0;
	return out;
}

static char *
_avr_vcd_get_signal_text(
		avr_vcd_signal_t * s,
		char * out,
		uint32_t value)
{
	char * dst = out;

	if (s->size > 1)
		*dst++ = 'b';

	for (int i = s->size; i > 0; i--)
		*dst++ = value & (1 << (i-1)) ? '1' : '0';
	if (s->size > 1)
		*dst++ = ' ';
	*dst++ = s->alias;
	*dst = 0;
	return out;
}

static void
avr_vcd_flush_log(
		avr_vcd_t * vcd)
{
#if AVR_VCD_MAX_SIGNALS > 32
	uint64_t seen = 0;
#else
	uint32_t seen = 0;
#endif
	uint64_t oldbase = 0;	// make sure it's different
	char out[48];

	if (!vcd->logindex || !vcd->output)
		return;
//	printf("avr_vcd_flush_log %d\n", vcd->logindex);

	for (uint32_t li = 0; li < vcd->logindex; li++) {
		avr_vcd_log_t *l = &vcd->log[li];
		uint64_t base = avr_cycles_to_nsec(vcd->avr, l->when - vcd->start);	// 1ns base

		// if that trace was seen in this nsec already, we fudge the base time
		// to make sure the new value is offset by one nsec, to make sure we get
		// at least a small pulse on the waveform
		// This is a bit of a fudge, but it is the only way to represent very
		// short "pulses" that are still visible on the waveform.
		if (base == oldbase && seen & (1 << l->signal->irq.irq))
			base++;	// this forces a new timestamp

		if (base > oldbase || li == 0) {
			seen = 0;
			fprintf(vcd->output, "#%" PRIu64  "\n", base);
			oldbase = base;
		}
		seen |= (1 << l->signal->irq.irq);	// mark this trace as seen for this timestamp
		fprintf(vcd->output, "%s\n", _avr_vcd_get_signal_text(l->signal, out, l->value));
	}
	vcd->logindex = 0;
}

static avr_cycle_count_t
_avr_vcd_timer(
		struct avr_t * avr,
		avr_cycle_count_t when,
		void * param)
{
	avr_vcd_t * vcd = param;
	avr_vcd_flush_log(vcd);
	return when + vcd->period;
}

int
avr_vcd_add_signal(
		avr_vcd_t * vcd,
		avr_irq_t * signal_irq,
		int signal_bit_size,
		const char * name )
{
	if (vcd->signal_count == AVR_VCD_MAX_SIGNALS)
		return -1;
	int index = vcd->signal_count++;
	avr_vcd_signal_t * s = &vcd->signal[index];
	strncpy(s->name, name, sizeof(s->name));
	s->size = signal_bit_size;
	s->alias = ' ' + vcd->signal_count ;

	/* manufacture a nice IRQ name */
	int l = strlen(name);
	char iname[10 + l + 1];
	if (signal_bit_size > 1)
		sprintf(iname, "%d>vcd.%s", signal_bit_size, name);
	else
		sprintf(iname, ">vcd.%s", name);

	const char * names[1] = { iname };
	avr_init_irq(&vcd->avr->irq_pool, &s->irq, index, 1, names);
	avr_irq_register_notify(&s->irq, _avr_vcd_notify, vcd);

	avr_connect_irq(signal_irq, &s->irq);
	return 0;
}


int
avr_vcd_start(
		avr_vcd_t * vcd)
{
	if (vcd->output)
		avr_vcd_stop(vcd);
	vcd->output = fopen(vcd->filename, "w");
	if (vcd->output == NULL) {
		perror(vcd->filename);
		return -1;
	}

	fprintf(vcd->output, "$timescale 1ns $end\n");	// 1ns base
	fprintf(vcd->output, "$scope module logic $end\n");

	for (int i = 0; i < vcd->signal_count; i++) {
		fprintf(vcd->output, "$var wire %d %c %s $end\n",
			vcd->signal[i].size, vcd->signal[i].alias, vcd->signal[i].name);
	}

	fprintf(vcd->output, "$upscope $end\n");
	fprintf(vcd->output, "$enddefinitions $end\n");

	fprintf(vcd->output, "$dumpvars\n");
	for (int i = 0; i < vcd->signal_count; i++) {
		avr_vcd_signal_t * s = &vcd->signal[i];
		char out[48];
		fprintf(vcd->output, "%s\n", _avr_vcd_get_float_signal_text(s, out));
	}
	fprintf(vcd->output, "$end\n");
	vcd->logindex = 0;
	vcd->start = vcd->avr->cycle;
	avr_cycle_timer_register(vcd->avr, vcd->period, _avr_vcd_timer, vcd);
	return 0;
}

int
avr_vcd_stop(
		avr_vcd_t * vcd)
{
	avr_cycle_timer_cancel(vcd->avr, _avr_vcd_timer, vcd);

	avr_vcd_flush_log(vcd);

	if (vcd->output)
		fclose(vcd->output);
	vcd->output = NULL;
	return 0;
}


