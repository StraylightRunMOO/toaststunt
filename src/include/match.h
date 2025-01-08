/******************************************************************************
  Copyright (c) 1992, 1995, 1996 Xerox Corporation.  All rights reserved.
  Portions of this code were written by Stephen White, aka ghond.
  Use and copying of this software and preparation of derivative works based
  upon this software are permitted.  Any distribution of this software or
  derivative works must comply with all applicable United States export
  control laws.  This software is made available AS IS, and Xerox Corporation
  makes no warranty about the software, its performance or its conformity to
  any specification.  Any person obtaining a copy of this software is requested
  to send their name and post office or electronic mail address to:
    Pavel Curtis
    Xerox PARC
    3333 Coyote Hill Rd.
    Palo Alto, CA 94304
    Pavel@Xerox.Com
 *****************************************************************************/


#ifndef EXT_MATCH_H
#define EXT_MATCH_H 1


#include "config.h"
#include "structures.h"
#include <regex>


extern Objid match_object(Objid player, const char *name);

// Regexes for ordinal parsing
static const std::unordered_map<std::string, int> english_ordinals = {{"first", 1}, {"second", 2}, {"third", 3}, {"fourth", 4}, {"fifth", 5}, {"sixth", 6}, {"seventh", 7}, {"eighth", 8}, {"ninth", 9}, {"tenth", 10}, {"eleventh", 11}, {"twelfth", 12}, {"thirteenth", 13}, {"fourteenth", 14}, {"fifteenth", 15}, {"sixteenth", 16}, {"seventeenth", 17}, {"eighteenth", 18}, {"nineteenth", 19}, {"twentieth", 20}, {"twenty-first", 21}, {"twenty-second", 22}, {"twenty-third", 23}, {"twenty-fourth", 24}, {"twenty-fifth", 25}, {"twenty-sixth", 26}, {"twenty-seventh", 27}, {"twenty-eighth", 28}, {"twenty-ninth", 29}, {"thirtieth", 30}, {"thirty-first", 31}, {"thirty-second", 32}, {"thirty-third", 33}, {"thirty-fourth", 34}, {"thirty-fifth", 35}, {"thirty-sixth", 36}, {"thirty-seventh", 37}, {"thirty-eighth", 38}, {"thirty-ninth", 39}, {"fortieth", 40}, {"forty-first", 41}, {"forty-second", 42}, {"forty-third", 43}, {"forty-fourth", 44}, {"forty-fifth", 45}, {"forty-sixth", 46}, {"forty-seventh", 47}, {"forty-eighth", 48}, {"forty-ninth", 49}, {"fiftieth", 50}, {"fifty-first", 51}, {"fifty-second", 52}, {"fifty-third", 53}, {"fifty-fourth", 54}, {"fifty-fifth", 55}, {"fifty-sixth", 56}, {"fifty-seventh", 57}, {"fifty-eighth", 58}, {"fifty-ninth", 59}, {"sixtieth", 60}, {"sixty-first", 61}, {"sixty-second", 62}, {"sixty-third", 63}, {"sixty-fourth", 64}, {"sixty-fifth", 65}, {"sixty-sixth", 66}, {"sixty-seventh", 67}, {"sixty-eighth", 68}, {"sixty-ninth", 69}, {"seventieth", 70}, {"seventy-first", 71}, {"seventy-second", 72}, {"seventy-third", 73}, {"seventy-fourth", 74}, {"seventy-fifth", 75}, {"seventy-sixth", 76}, {"seventy-seventh", 77}, {"seventy-eighth", 78}, {"seventy-ninth", 79}, {"eightieth", 80}, {"eighty-first", 81}, {"eighty-second", 82}, {"eighty-third", 83}, {"eighty-fourth", 84}, {"eighty-fifth", 85}, {"eighty-sixth", 86}, {"eighty-seventh", 87}, {"eighty-eighth", 88}, {"eighty-ninth", 89}, {"ninetieth", 90}, {"ninety-first", 91}, {"ninety-second", 92}, {"ninety-third", 93}, {"ninety-fourth", 94}, {"ninety-fifth", 95}, {"ninety-sixth", 96}, {"ninety-seventh", 97}, {"ninety-eighth", 98}, {"ninety-ninth", 99}};
static const std::regex match_numeric ("^(\\d+)\\.");
static const std::regex match_ordinal ("^(\\d+)(th|st|nd|rd)");

#endif
