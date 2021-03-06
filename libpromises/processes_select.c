/*
   Copyright (C) CFEngine AS

   This file is part of CFEngine 3 - written and maintained by CFEngine AS.

   This program is free software; you can redistribute it and/or modify it
   under the terms of the GNU General Public License as published by the
   Free Software Foundation; version 3.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA

  To the extent this program is licensed as part of the Enterprise
  versions of CFEngine, the applicable Commercial Open Source License
  (COSL) may apply to this file if you as a licensee so wish it. See
  included file COSL.txt.
*/

#include <processes_select.h>

#include <string.h>

#include <eval_context.h>
#include <files_names.h>
#include <conversion.h>
#include <matching.h>
#include <systype.h>
#include <string_lib.h>                                         /* Chop */
#include <regex.h> /* CompileRegex,StringMatchWithPrecompiledRegex,StringMatchFull */
#include <item_lib.h>
#include <pipes.h>
#include <files_interfaces.h>
#include <rlist.h>
#include <policy.h>
#include <zones.h>
#include <printsize.h>

# ifdef HAVE_GETZONEID
#include <sequence.h>
#define MAX_ZONENAME_SIZE 64
# endif

static int SelectProcRangeMatch(char *name1, char *name2, int min, int max, char **names, char **line);
static bool SelectProcRegexMatch(const char *name1, const char *name2, const char *regex, char **colNames, char **line);
static int SplitProcLine(const char *proc, char **names, int *start, int *end, char **line);
static int SelectProcTimeCounterRangeMatch(char *name1, char *name2, time_t min, time_t max, char **names, char **line);
static int SelectProcTimeAbsRangeMatch(char *name1, char *name2, time_t min, time_t max, char **names, char **line);
static int GetProcColumnIndex(const char *name1, const char *name2, char **names);
static void GetProcessColumnNames(char *proc, char **names, int *start, int *end);
static int ExtractPid(char *psentry, char **names, int *end);

/***************************************************************************/

static int SelectProcess(char *procentry, char **names, int *start, int *end, ProcessSelect a)
{
    int result = true, i;
    char *column[CF_PROCCOLS];
    Rlist *rp;

    StringSet *process_select_attributes = StringSetNew();

    if (!SplitProcLine(procentry, names, start, end, column))
    {
        return false;
    }

    for (i = 0; names[i] != NULL; i++)
    {
        Log(LOG_LEVEL_DEBUG, "In SelectProcess, COL[%s] = '%s'", names[i], column[i]);
    }

    for (rp = a.owner; rp != NULL; rp = rp->next)
    {
        if (SelectProcRegexMatch("USER", "UID", RlistScalarValue(rp), names, column))
        {
            StringSetAdd(process_select_attributes, xstrdup("process_owner"));
            break;
        }
    }

    if (SelectProcRangeMatch("PID", "PID", a.min_pid, a.max_pid, names, column))
    {
        StringSetAdd(process_select_attributes, xstrdup("pid"));
    }

    if (SelectProcRangeMatch("PPID", "PPID", a.min_ppid, a.max_ppid, names, column))
    {
        StringSetAdd(process_select_attributes, xstrdup("ppid"));
    }

    if (SelectProcRangeMatch("PGID", "PGID", a.min_pgid, a.max_pgid, names, column))
    {
        StringSetAdd(process_select_attributes, xstrdup("pgid"));
    }

    if (SelectProcRangeMatch("VSZ", "SZ", a.min_vsize, a.max_vsize, names, column))
    {
        StringSetAdd(process_select_attributes, xstrdup("vsize"));
    }

    if (SelectProcRangeMatch("RSS", "RSS", a.min_rsize, a.max_rsize, names, column))
    {
        StringSetAdd(process_select_attributes, xstrdup("rsize"));
    }

    if (SelectProcTimeCounterRangeMatch("TIME", "TIME", a.min_ttime, a.max_ttime, names, column))
    {
        StringSetAdd(process_select_attributes, xstrdup("ttime"));
    }

    if (SelectProcTimeAbsRangeMatch
        ("STIME", "START", a.min_stime, a.max_stime, names, column))
    {
        StringSetAdd(process_select_attributes, xstrdup("stime"));
    }

    if (SelectProcRangeMatch("NI", "PRI", a.min_pri, a.max_pri, names, column))
    {
        StringSetAdd(process_select_attributes, xstrdup("priority"));
    }

    if (SelectProcRangeMatch("NLWP", "NLWP", a.min_thread, a.max_thread, names, column))
    {
        StringSetAdd(process_select_attributes, xstrdup("threads"));
    }

    if (SelectProcRegexMatch("S", "STAT", a.status, names, column))
    {
        StringSetAdd(process_select_attributes, xstrdup("status"));
    }

    if (SelectProcRegexMatch("CMD", "COMMAND", a.command, names, column))
    {
        StringSetAdd(process_select_attributes, xstrdup("command"));
    }

    if (SelectProcRegexMatch("TTY", "TTY", a.tty, names, column))
    {
        StringSetAdd(process_select_attributes, xstrdup("tty"));
    }

    if (!a.process_result)
    {
        if (StringSetSize(process_select_attributes) == 0)
        {
            result = EvalProcessResult("", process_select_attributes);
        }
        else
        {
            Writer *w = StringWriter();
            StringSetIterator iter = StringSetIteratorInit(process_select_attributes);
            char *attr = StringSetIteratorNext(&iter);
            WriterWrite(w, attr);

            while ((attr = StringSetIteratorNext(&iter)))
            {
                WriterWriteChar(w, '.');
                WriterWrite(w, attr);
            }

            result = EvalProcessResult(StringWriterData(w), process_select_attributes);
            WriterClose(w);
        }
    }
    else
    {
        result = EvalProcessResult(a.process_result, process_select_attributes);
    }

    StringSetDestroy(process_select_attributes);

    for (i = 0; column[i] != NULL; i++)
    {
        free(column[i]);
    }

    return result;
}

Item *SelectProcesses(const Item *processes, const char *process_name, ProcessSelect a, bool attrselect)
{
    Item *result = NULL;

    if (processes == NULL)
    {
        return result;
    }

    char *names[CF_PROCCOLS];
    int start[CF_PROCCOLS];
    int end[CF_PROCCOLS];

    GetProcessColumnNames(processes->name, &names[0], start, end);

    pcre *rx = CompileRegex(process_name);
    if (rx)
    {
        for (Item *ip = processes->next; ip != NULL; ip = ip->next)
        {
            int s, e;

            if (StringMatchWithPrecompiledRegex(rx, ip->name, &s, &e))
            {
                if (NULL_OR_EMPTY(ip->name))
                {
                    continue;
                }

                if (attrselect && !SelectProcess(ip->name, names, start, end, a))
                {
                    continue;
                }

                pid_t pid = ExtractPid(ip->name, names, end);

                if (pid == -1)
                {
                    Log(LOG_LEVEL_VERBOSE, "Unable to extract pid while looking for %s", process_name);
                    continue;
                }

                PrependItem(&result, ip->name, "");
                result->counter = (int)pid;
            }
        }

        pcre_free(rx);
    }

    for (int i = 0; i < CF_PROCCOLS; i++)
    {
        free(names[i]);
    }

    return result;
}

static int SelectProcRangeMatch(char *name1, char *name2, int min, int max, char **names, char **line)
{
    int i;
    long value;

    if ((min == CF_NOINT) || (max == CF_NOINT))
    {
        return false;
    }

    if ((i = GetProcColumnIndex(name1, name2, names)) != -1)
    {
        value = IntFromString(line[i]);

        if (value == CF_NOINT)
        {
            Log(LOG_LEVEL_INFO, "Failed to extract a valid integer from '%s' => '%s' in process list", names[i],
                  line[i]);
            return false;
        }

        if ((min <= value) && (value <= max))
        {
            return true;
        }
        else
        {
            return false;
        }
    }

    return false;
}

/***************************************************************************/

static long TimeCounter2Int(const char *s)
{
    long days, hours, minutes, seconds;

    if (s == NULL)
    {
        return CF_NOINT;
    }

    /* If we match dd-hh:mm[:ss], believe it: */
    int got = sscanf(s, "%ld-%ld:%ld:%ld", &days, &hours, &minutes, &seconds);
    if (got > 2)
    {
        /* All but perhaps seconds set */
    }
    /* Failing that, try matching hh:mm[:ss] */
    else if (1 < (got = sscanf(s, "%ld:%ld:%ld", &hours, &minutes, &seconds)))
    {
        /* All but days and perhaps seconds set */
        days = 0;
        got++;
    }
    else
    {
        Log(LOG_LEVEL_ERR,
            "Unable to parse 'ps' time field as [dd-]hh:mm[:ss], got '%s'",
            s);
        return CF_NOINT;
    }
    assert(got > 2); /* i.e. all but maybe seconds have been set */
    /* Clear seconds if unset: */
    if (got < 4)
    {
        seconds = 0;
    }

    Log(LOG_LEVEL_DEBUG,
        "TimeCounter2Int: Parsed '%s' as elapsed time '%ld-%02ld:%02ld:%02ld'",
        s, days, hours, minutes, seconds);

    /* Convert to seconds: */
    return ((days * 24 + hours) * 60 + minutes) * 60 + seconds;
}

static int SelectProcTimeCounterRangeMatch(char *name1, char *name2, time_t min, time_t max, char **names, char **line)
{
    if ((min == CF_NOINT) || (max == CF_NOINT))
    {
        return false;
    }

    int i = GetProcColumnIndex(name1, name2, names);
    if (i != -1)
    {
        time_t value = (time_t) TimeCounter2Int(line[i]);

        if (value == CF_NOINT)
        {
            Log(LOG_LEVEL_INFO, "Failed to extract a valid integer from %c => '%s' in process list", name1[i],
                  line[i]);
            return false;
        }

        if ((min <= value) && (value <= max))
        {
            Log(LOG_LEVEL_VERBOSE, "Selection filter matched counter range '%s/%s' = '%s' in [%jd,%jd] (= %jd secs)",
                  name1, name2, line[i], (intmax_t)min, (intmax_t)max, (intmax_t)value);
            return true;
        }
        else
        {
            Log(LOG_LEVEL_DEBUG,
                "Selection filter REJECTED counter range '%s/%s' = '%s' in [%jd,%jd] (= %jd secs)",
                name1, name2, line[i],
                (intmax_t) min, (intmax_t) max, (intmax_t) value);
            return false;
        }
    }

    return false;
}

static time_t TimeAbs2Int(const char *s)
{
    if (s == NULL)
    {
        return CF_NOINT;
    }

    struct tm tm;
    localtime_r(&CFSTARTTIME, &tm);
    tm.tm_sec = 0;
    tm.tm_isdst = -1;

    /* Try various ways to parse s: */
    char word[4]; /* Abbreviated month name */
    long ns[3]; /* Miscellaneous numbers, diverse readings */
    int got = sscanf(s, "%2ld:%2ld:%2ld", ns, ns + 1, ns + 2);
    if (1 < got) /* Hr:Min[:Sec] */
    {
        tm.tm_hour = ns[0];
        tm.tm_min = ns[1];
        if (got == 3)
        {
            tm.tm_sec = ns[2];
        }
    }
    /* or MMM dd (the %ld shall ignore any leading space) */
    else if (2 == sscanf(s, "%3[a-zA-Z]%ld", word, ns) &&
             /* Only match if word is a valid month text: */
             0 < (ns[1] = Month2Int(word)))
    {
        int month = ns[1] - 1;
        if (tm.tm_mon < month)
        {
            /* Wrapped around */
            tm.tm_year--;
        }
        tm.tm_mon = month;
        tm.tm_mday = ns[0];
        tm.tm_hour = 0;
        tm.tm_min = 0;
    }
    /* or just year, or seconds since 1970 */
    else if (1 == sscanf(s, "%ld", ns))
    {
        if (ns[0] > 9999)
        {
            /* Seconds since 1970.
             *
             * This is the amended value SplitProcLine() replaces
             * start time with if it's imprecise and a better value
             * can be calculated from elapsed time.
             */
            return (time_t)ns[0];
        }
        /* else year, at most four digits; either 4-digit CE or
         * already relative to 1900. */

        memset(&tm, 0, sizeof(tm));
        tm.tm_year = ns[0] < 999 ? ns[0] : ns[0] - 1900;
        tm.tm_isdst = -1;
    }
    else
    {
        return CF_NOINT;
    }

    return mktime(&tm);
}

static int SelectProcTimeAbsRangeMatch(char *name1, char *name2, time_t min, time_t max, char **names, char **line)
{
    int i;
    time_t value;

    if ((min == CF_NOINT) || (max == CF_NOINT))
    {
        return false;
    }

    if ((i = GetProcColumnIndex(name1, name2, names)) != -1)
    {
        value = TimeAbs2Int(line[i]);

        if (value == CF_NOINT)
        {
            Log(LOG_LEVEL_INFO, "Failed to extract a valid integer from %c => '%s' in process list", name1[i],
                  line[i]);
            return false;
        }

        if ((min <= value) && (value <= max))
        {
            Log(LOG_LEVEL_VERBOSE, "Selection filter matched absolute '%s/%s' = '%s(%jd)' in [%jd,%jd]", name1, name2, line[i], (intmax_t)value,
                  (intmax_t)min, (intmax_t)max);
            return true;
        }
        else
        {
            return false;
        }
    }

    return false;
}

/***************************************************************************/

static bool SelectProcRegexMatch(const char *name1, const char *name2,
                                 const char *regex, char **colNames, char **line)
{
    int i;

    if (regex == NULL)
    {
        return false;
    }

    if ((i = GetProcColumnIndex(name1, name2, colNames)) != -1)
    {

        if (StringMatchFull(regex, line[i]))
        {
            return true;
        }
        else
        {
            return false;
        }
    }

    return false;
}

/*******************************************************************/
/* line must be char *line[CF_PROCCOLS] in fact. */

static int SplitProcLine(const char *proc,
                         char **names, int *start, int *end,
                         char **line)
{
    if (proc == NULL || proc[0] == '\0')
    {
        return false;
    }

    memset(line, 0, sizeof(char *) * CF_PROCCOLS);

    const char *sp = proc;
    /* Scan in parallel for two heuristics: space-delimited fields
     * found using sp, and ones inferred from the column headers. */

    for (int i = 0; i < CF_PROCCOLS && names[i] != NULL; i++)
    {
        /* Space-delimited heuristic, from sp to just before ep: */
        while (isspace((unsigned char) sp[0]))
        {
            sp++;
        }
        const char *ep = sp;

        /* Header-driven heuristic, field from proc[s] to proc[e].
         * Start with the column header's position and maybe grow
         * outwards. */
        int s = start[i], e;
        if (i + 1 == CF_PROCCOLS || names[i + 1] == NULL)
        {
            e = strlen(proc) - 1;
            /* Extend space-delimited field to line end: */
            while (ep[0] && ep[0] != '\n')
            {
                ep++;
            }
            /* But trim trailing hspace: */
            while (isspace((unsigned char)ep[-1]))
            {
                ep--;
            }
        }
        else
        {
            e = end[i];
            /* Extend space-delimited to next space: */
            while (ep[0] && !isspace((unsigned char)ep[0]))
            {
                ep++;
            }
        }
        /* ep points at the space (or '\0') *following* the word or
         * final field. */

        /* Some ps stimes may contain spaces, e.g. "Jan 25" */
        if (strcmp(names[i], "STIME") == 0 &&
            ep - sp == 3)
        {
            const char *np = ep;
            while (isspace((unsigned char) np[0]))
            {
                np++;
            }
            if (isdigit((unsigned char) np[0]))
            {
                do
                {
                    np++;
                }
                while (isdigit((unsigned char) np[0]));
                ep = np;
            }
        }

        /* Numeric columns, including times, are apt to grow leftwards
         * and be right-aligned; identify candidates for this by
         * proc[e] being a digit.  Text columns are typically
         * left-aligned and may grow rightwards; identify candidates
         * for this by proc[s] being alphabetic.  Some columns shall
         * match both (e.g. STIME).  Some numeric columns may grow
         * left even as far as under the heading of the next column
         * (seen with ps -fel's SZ spilling left into ADDR's space on
         * Linux).  While widening, it's OK to include a stray space;
         * we'll trim that afterwards. */

        /* Move s left until we run outside the field or find space. */
        if (isdigit((unsigned char)proc[e]))
        {
            /* Numeric fields may include punctuators: */
#define IsNumberish(c) (isdigit((unsigned char)(c)) || ispunct((unsigned char)(c)))

            bool number = i > 0; /* Should we check for under-spill ? */
            int outer = number ? end[i - 1] + 1 : 0;
            while (s >= outer && !isspace((unsigned char) proc[s]))
            {
                if (number && !IsNumberish(proc[s]))
                {
                    number = false;
                }
                s--;
            }

            /* Numeric field might overspill under previous header: */
            if (s < outer)
            {
                int spill = s;
                s = outer; /* By default, don't overlap previous column. */

                if (number && IsNumberish(proc[spill]))
                {
                    outer = start[i - 1];
                    /* Explore all the way to the start-column of the
                     * previous field's header.  If we get there, in
                     * digits-and-punctuation, we've got two numeric
                     * fields that abut; we can't do better than assume
                     * the boundary is under the right end of the previous
                     * column label (which is what our parsing of the
                     * previous column assumed).  So leave s where it is,
                     * just after the previous field's header's
                     * end-column. */

                    while (spill > outer)
                    {
                        spill--;
                        if (!IsNumberish(proc[spill]))
                        {
                            s = spill + 1; /* Confirmed overlap. */
                            break;
                        }
                    }
                }
            }
#undef IsNumberish
        }

        /* Move e right likewise (but never under next heading): */
        if (isalpha((unsigned char)proc[s]))
        {
            int outer;
            if (i + 1 < CF_PROCCOLS && names[i + 1])
            {
                outer = start[i + 1];
            }
            else
            {
                outer = strlen(proc);
            }
            /* Simplify loop condition; we want e to end *before* next
             * field, not on its first byte (or the terminator): */
            outer--;

            while (e < outer && !isspace((unsigned char) proc[e]))
            {
                e++;
            }
        }

        /* Strip off any leading and trailing spaces: */
        while (isspace((unsigned char) proc[s]))
        {
            s++;
        }
        /* ... but stop if the field is empty ! */
        while (s <= e && isspace((unsigned char) proc[e]))
        {
            e--;
        }

        /* Grumble if the two heuristics don't agree: */
        size_t wordlen = ep - sp;
        if (e < s ? ep > sp : (sp != proc + s || ep != proc + e + 1))
        {
            char word[CF_SMALLBUF];
            if (wordlen >= CF_SMALLBUF)
            {
                wordlen = CF_SMALLBUF - 1;
            }
            memcpy(word, sp, wordlen);
            word[wordlen] = '\0';

            char column[CF_SMALLBUF];
            if (s <= e)
            {
                /* Copy proc[s through e] inclusive:  */
                const size_t len = MIN(1 + e - s, CF_SMALLBUF - 1);
                memcpy(column, proc + s, len);
                column[len] = '\0';
            }
            else
            {
                column[0] = '\0';
            }

            Log(LOG_LEVEL_INFO,
                "Unacceptable model uncertainty examining process(%s): '%s' != '%s'",
                proc, word, column);
        }

        /* Fall back on word if column got an empty answer: */
        line[i] = e < s ? xstrndup(sp, ep - sp) : xstrndup(proc + s, 1 + e - s);
        sp = ep;
    }

    /* Since start times can be very imprecise (e.g. just a past day's
     * date, or a past year), calculate a better value from elapsed
     * time, if available: */
    int k = GetProcColumnIndex("ELAPSED", "ELAPSED", names);
    if (k != -1)
    {
        const long elapsed = TimeCounter2Int(line[k]);
        if (elapsed != CF_NOINT) /* Only use if parsed successfully ! */
        {
            int j = GetProcColumnIndex("STIME", "START", names), ns[3];
            /* Trust the reported value if it matches hh:mm[:ss], though: */
            if (sscanf(line[j], "%d:%d:%d", ns, ns + 1, ns + 2) < 2)
            {
                /* TODO: use time of ps-run, not time(NULL), which may be later. */
                time_t value = time(NULL) - (time_t) elapsed;

                Log(LOG_LEVEL_DEBUG,
                    "SplitProcLine: Replacing parsed start time %s with %s",
                    line[j], ctime(&value));

                free(line[j]);
                xasprintf(line + j, "%ld", value);
            }
        }
    }

    return true;
}

/*******************************************************************/

static int GetProcColumnIndex(const char *name1, const char *name2, char **names)
{
    for (int i = 0; names[i] != NULL; i++)
    {
        if (strcmp(names[i], name1) == 0 ||
            strcmp(names[i], name2) == 0)
        {
            return i;
        }
    }

    Log(LOG_LEVEL_VERBOSE,
        "Process column %s/%s was not supported on this system",
        name1, name2);

    return -1;
}

/**********************************************************************************/

bool IsProcessNameRunning(char *procNameRegex)
{
    char *colHeaders[CF_PROCCOLS];
    int start[CF_PROCCOLS] = { 0 };
    int end[CF_PROCCOLS] = { 0 };
    bool matched = false;
    int i;

    if (PROCESSTABLE == NULL)
    {
        Log(LOG_LEVEL_ERR, "IsProcessNameRunning: PROCESSTABLE is empty");
        return false;
    }

    GetProcessColumnNames(PROCESSTABLE->name, (char **) colHeaders, start, end);

    for (const Item *ip = PROCESSTABLE->next; !matched && ip != NULL; ip = ip->next) // iterate over ps lines
    {
        char *lineSplit[CF_PROCCOLS];

        if (NULL_OR_EMPTY(ip->name))
        {
            continue;
        }

        if (!SplitProcLine(ip->name, colHeaders, start, end, lineSplit))
        {
            Log(LOG_LEVEL_ERR, "IsProcessNameRunning: Could not split process line '%s'", ip->name);
            continue;
        }

        if (SelectProcRegexMatch("CMD", "COMMAND", procNameRegex, colHeaders, lineSplit))
        {
            matched = true;
        }

        for (i = 0; lineSplit[i] != NULL; i++)
        {
            free(lineSplit[i]);
        }
    }

    for (i = 0; colHeaders[i] != NULL; i++)
    {
        free(colHeaders[i]);
    }

    return matched;
}


static void GetProcessColumnNames(char *proc, char **names, int *start, int *end)
{
    char *sp, title[16];
    int col, offset = 0;

    for (col = 0; col < CF_PROCCOLS; col++)
    {
        start[col] = end[col] = -1;
        names[col] = NULL;
    }

    col = 0;

    for (sp = proc; *sp != '\0'; sp++)
    {
        offset = sp - proc;

        if (isspace((unsigned char) *sp))
        {
            if (start[col] != -1)
            {
                Log(LOG_LEVEL_DEBUG, "End of '%s' is %d", title, offset - 1);
                end[col++] = offset - 1;
                if (col > CF_PROCCOLS - 1)
                {
                    Log(LOG_LEVEL_ERR, "Column overflow in process table");
                    break;
                }
            }
        }
        else if (start[col] == -1)
        {
            start[col] = offset;
            sscanf(sp, "%15s", title);
            Log(LOG_LEVEL_DEBUG, "Start of '%s' is %d", title, offset);
            names[col] = xstrdup(title);
            Log(LOG_LEVEL_DEBUG, "Col[%d] = '%s'", col, names[col]);
        }
    }

    if (end[col] == -1)
    {
        Log(LOG_LEVEL_DEBUG, "End of '%s' is %d", title, offset);
        end[col] = offset;
    }
}

#ifndef __MINGW32__
static const char *GetProcessOptions(void)
{

# ifdef __linux__
    if (strncmp(VSYSNAME.release, "2.4", 3) == 0)
    {
        // No threads on 2.4 kernels
        return "-eo user,pid,ppid,pgid,pcpu,pmem,vsz,pri,rss,stime,time,args";
    }
# endif

    return VPSOPTS[VSYSTEMHARDCLASS];
}
#endif

static int ExtractPid(char *psentry, char **names, int *end)
{
    int offset = 0;

    for (int col = 0; col < CF_PROCCOLS; col++)
    {
        if (strcmp(names[col], "PID") == 0)
        {
            if (col > 0)
            {
                offset = end[col - 1];
            }
            break;
        }
    }

    for (const char *sp = psentry + offset; *sp != '\0'; sp++) /* if first field contains alpha, skip */
    {
        /* If start with alphanum then skip it till the first space */

        if (isalnum((unsigned char) *sp))
        {
            while (*sp != ' ' && *sp != '\0')
            {
                sp++;
            }
        }

        while (*sp == ' ' || *sp == '\t')
        {
            sp++;
        }

        int pid;
        if (sscanf(sp, "%d", &pid) == 1 && pid != -1)
        {
            return pid;
        }
    }

    return -1;
}

# ifndef __MINGW32__
# ifdef HAVE_GETZONEID
/* ListLookup with the following return semantics
 * -1 if the first argument is smaller than the second
 *  0 if the arguments are equal
 *  1 if the first argument is bigger than the second
 */
int PidListCompare(const void *pid1, const void *pid2, ARG_UNUSED void *user_data)
{
    int p1 = (intptr_t)(void *)pid1;
    int p2 = (intptr_t)(void *)pid2;

    if (p1 < p2)
    {
        return -1;
    }
    else if (p1 > p2)
    {
        return 1;
    }
    return 0;
}
/* Load processes using zone-aware ps
 * to obtain solaris list of global
 * process ids for root and non-root
 * users to lookup later */
int ZLoadProcesstable(Seq *pidlist, Seq *rootpidlist)
{

    char *names[CF_PROCCOLS];
    int start[CF_PROCCOLS];
    int end[CF_PROCCOLS];

    int index = 0;
    const char *pscmd = "/usr/bin/ps -Aleo zone,user,pid";

    FILE *psf = cf_popen(pscmd, "r", false);
    if (psf == NULL)
    {
        Log(LOG_LEVEL_ERR, "ZLoadProcesstable: Couldn't open the process list with command %s.", pscmd);
        return false;
    }

    size_t pbuff_size = CF_BUFSIZE;
    char *pbuff = xmalloc(pbuff_size);

    while (true)
    {
        ssize_t res = CfReadLine(&pbuff, &pbuff_size, psf);
        if (res == -1)
        {
            if (!feof(psf))
            {
                Log(LOG_LEVEL_ERR, "IsGlobalProcess(char **, int): Unable to read process list with command '%s'. (fread: %s)", pscmd, GetErrorStr());
                cf_pclose(psf);
                free(pbuff);
                return false;
            }
            else
            {
                break;
            }
        }
        Chop(pbuff, pbuff_size);
        if (strstr(pbuff, "PID")) /*this is the banner*/
        {
            GetProcessColumnNames(pbuff, &names[0], start, end);
        }
        else
        {
            int pid = ExtractPid(pbuff, &names[0], end);

            size_t zone_offset = strspn(pbuff, " ");
            size_t zone_end_offset = strcspn(pbuff + zone_offset, " ") + zone_offset;
            size_t user_offset = strspn(pbuff + zone_end_offset, " ") + zone_end_offset;
            size_t user_end_offset = strcspn(pbuff + user_offset, " ") + user_offset;
            bool is_global = (zone_end_offset - zone_offset == 6
                                  && strncmp(pbuff + zone_offset, "global", 6) == 0);
            bool is_root = (user_end_offset - user_offset == 4
                                && strncmp(pbuff + user_offset, "root", 4) == 0);

            if (is_global && is_root)
            {
                SeqAppend(rootpidlist, (void*)(intptr_t)pid);
            }
            else if (is_global && !is_root)
            {
                SeqAppend(pidlist, (void*)(intptr_t)pid);
            }
        }
    }
    cf_pclose(psf);
    free(pbuff);
    return true;
}
bool PidInSeq(Seq *list, int pid)
{
    void *res = SeqLookup(list, (void *)(intptr_t)pid, PidListCompare);
    int result = (intptr_t)(void*)res;

    if (result == pid)
    {
        return true;
    }
    return false;
}
/* return true if the process with
 * pid is in the global zone */
int IsGlobalProcess(int pid, Seq *pidlist, Seq *rootpidlist)
{
    if (PidInSeq(pidlist, pid) || PidInSeq(rootpidlist, pid))
    {
       return true;
    }
    else
    {
       return false;
    }
}
void ZCopyProcessList(Item **dest, const Item *source, Seq *pidlist, char **names, int *end)
{
    int gpid = ExtractPid(source->name, names, end);

    if (PidInSeq(pidlist, gpid))
    {
        PrependItem(dest, source->name, "");
    }
}
# endif /* HAVE_GETZONEID */
int LoadProcessTable(Item **procdata)
{
    FILE *prp;
    char pscomm[CF_MAXLINKSIZE];
    Item *rootprocs = NULL;
    Item *otherprocs = NULL;


    if (PROCESSTABLE)
    {
        Log(LOG_LEVEL_VERBOSE, "Reusing cached process table");
        return true;
    }

    const char *psopts = GetProcessOptions();

    snprintf(pscomm, CF_MAXLINKSIZE, "%s %s", VPSCOMM[VSYSTEMHARDCLASS], psopts);

    Log(LOG_LEVEL_VERBOSE, "Observe process table with %s", pscomm);

    if ((prp = cf_popen(pscomm, "r", false)) == NULL)
    {
        Log(LOG_LEVEL_ERR, "Couldn't open the process list with command '%s'. (popen: %s)", pscomm, GetErrorStr());
        return false;
    }

    size_t vbuff_size = CF_BUFSIZE;
    char *vbuff = xmalloc(vbuff_size);

# ifdef HAVE_GETZONEID

    char *names[CF_PROCCOLS];
    int start[CF_PROCCOLS];
    int end[CF_PROCCOLS];
    Seq *pidlist = SeqNew(1, NULL);
    Seq *rootpidlist = SeqNew(1, NULL);
    bool global_zone = IsGlobalZone();

    if (global_zone)
    {
        int res = ZLoadProcesstable(pidlist, rootpidlist);

        if (res == false)
        {
            Log(LOG_LEVEL_ERR, "Unable to load solaris zone process table.");
            return false;
        }
    }

# endif

    for (;;)
    {
        ssize_t res = CfReadLine(&vbuff, &vbuff_size, prp);
        if (res == -1)
        {
            if (!feof(prp))
            {
                Log(LOG_LEVEL_ERR, "Unable to read process list with command '%s'. (fread: %s)", pscomm, GetErrorStr());
                cf_pclose(prp);
                free(vbuff);
                return false;
            }
            else
            {
                break;
            }
        }
        Chop(vbuff, vbuff_size);

# ifdef HAVE_GETZONEID

        if (global_zone)
        {
            if (strstr(vbuff, "PID") != NULL)
            {   /* this is the banner so get the column header names for later use*/
                GetProcessColumnNames(vbuff, &names[0], start, end);
            }
            else
            {
               int gpid = ExtractPid(vbuff, names, end);

               if (!IsGlobalProcess(gpid, pidlist, rootpidlist))
               {
                    continue;
               }
            }
        }

# endif
        AppendItem(procdata, vbuff, "");
    }

    cf_pclose(prp);

/* Now save the data */

    snprintf(vbuff, CF_MAXVARSIZE, "%s/state/cf_procs", CFWORKDIR);
    RawSaveItemList(*procdata, vbuff, NewLineMode_Unix);

# ifdef HAVE_GETZONEID
    if (global_zone) /* pidlist and rootpidlist are empty if we're not in the global zone */
    {
        Item *ip = *procdata;
        while (ip != NULL)
        {
            ZCopyProcessList(&rootprocs, ip, rootpidlist, names, end);
            ip = ip->next;
        }
        ReverseItemList(rootprocs);
        ip = *procdata;
        while (ip != NULL)
        {
            ZCopyProcessList(&otherprocs, ip, pidlist, names, end);
            ip = ip->next;
        }
        ReverseItemList(otherprocs);
    }
    else
# endif
    {
        CopyList(&rootprocs, *procdata);
        CopyList(&otherprocs, *procdata);

        while (DeleteItemNotContaining(&rootprocs, "root"))
        {
        }

        while (DeleteItemContaining(&otherprocs, "root"))
        {
        }
    }
    if (otherprocs)
    {
        PrependItem(&rootprocs, otherprocs->name, NULL);
    }
    snprintf(vbuff, CF_MAXVARSIZE, "%s/state/cf_rootprocs", CFWORKDIR);
    RawSaveItemList(rootprocs, vbuff, NewLineMode_Unix);
    DeleteItemList(rootprocs);

    snprintf(vbuff, CF_MAXVARSIZE, "%s/state/cf_otherprocs", CFWORKDIR);
    RawSaveItemList(otherprocs, vbuff, NewLineMode_Unix);
    DeleteItemList(otherprocs);

    free(vbuff);
    return true;
}
# endif
