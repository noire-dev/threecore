/*
===========================================================================
Copyright (C) 1999-2005 Id Software, Inc.

This file is part of Quake III Arena source code.

Quake III Arena source code is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of the License,
or (at your option) any later version.

Quake III Arena source code is distributed in the hope that it will be
useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Quake III Arena source code; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
===========================================================================
*/

#include "client.h"

#define CON_MAXLINES 16384
#define CON_MAXLINE 256
#define CON_PURGE_AMOUNT 1024

char con_lines[CON_MAXLINES][CON_MAXLINE];
int con_linescount = 0;

void CL_ConsolePrint(const char *txt) {
    if (!txt) return;
    
    if (con_linescount >= CON_MAXLINES) {
        for (int i = 0; i < CON_MAXLINES - CON_PURGE_AMOUNT; i++) memcpy(con_lines[i], con_lines[i + CON_PURGE_AMOUNT], CON_MAXLINE);
        for (int i = CON_MAXLINES - CON_PURGE_AMOUNT; i < CON_MAXLINES; i++) con_lines[i][0] = '\0';
        con_linescount = CON_MAXLINES - CON_PURGE_AMOUNT;
    }
    
    char formatted[CON_MAXLINE];
    qtime_t realtime;
    
    Com_RealTime(&realtime);
    snprintf(formatted, sizeof(formatted), "[%02d:%02d]  %s", realtime.tm_hour, realtime.tm_min, txt);
    
    strncpy(con_lines[con_linescount], formatted, CON_MAXLINE - 1);
    con_lines[con_linescount][CON_MAXLINE - 1] = '\0';
    con_linescount++;
}
