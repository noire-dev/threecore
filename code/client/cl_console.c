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

console_t con;

void CL_ConsolePrint(const char *txt) {
    if (!txt) return;
    
    if (con.linescount >= CON_MAXLINES) {
        for (int i = 0; i < CON_MAXLINES - CON_PURGE_AMOUNT; i++) memcpy(con.lines[i], con.lines[i + CON_PURGE_AMOUNT], CON_MAXLINE);
        for (int i = CON_MAXLINES - CON_PURGE_AMOUNT; i < CON_MAXLINES; i++) con.lines[i][0] = '\0';
        con.linescount = CON_MAXLINES - CON_PURGE_AMOUNT;
    }
    
    strncpy(con.lines[con.linescount], txt, CON_MAXLINE - 1);
    con.linescount++;
}

void CL_ConsoleSync(console_t* vmconsole, int currentLines) {
    if(con.linescount != currentLines) memcpy(vmconsole, &con, sizeof(console_t));
}
