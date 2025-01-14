# DBV_18 Release Notes

**NOTE:** This document is not yet complete. Documentation for DBV_18 updates:
  - New options (compile-time and $server_options)
  - ~~New primitive types (TYPE_CALL and TYPE_COMPLEX)~~ (partially documented)
  - ~~New and deprecated built-in functions~~
  - General math updates
  - DB File Changes
  - Breadth-wise multiple inheritance
  - Memento objects
  - Dynamic / ordinal object matching
  - New RT_ENV variables
  - Type system updates

```c
// Handy debug function used in examples below
// Usage: ;$debug(random(100), 3.1412, E_PERM, "some string", ["uno" -> 1, "dos" -> 2])
@verb #0:debug this none this
@prog #0:debug
c = callers(1);
cmsg = tostr(tostr(c[1][1]) + ":" + tostr(c[1][2]));
o = c[1][1];
v = tostr(c[1][2]);
if (`!valid(o) && v == "" ! ANY => true')
  caller = "EVAL";
  msg = tostr(" [caller=> " + caller + ", " + "line " + tostr(c[1][6]) + "]");
  return c[1][3]:tell(tostr("DEBUG=>  " + tostr(toliteral(args) + msg) + ""));
endif
caller = typeof(c[1][1]) == OBJ ? $code_utils:corify_object(c[1][1]) | $code_utils:corify_object(c[1][1].class);
caller = tostr(caller, "::", tostr(c[1][2]));
msg = tostr(" [caller=> " + caller + ", " + "line " + tostr(c[1][6]) + "]");
for dude in (connected_players())
  if (dude.wizard)
    dude:tell(tostr("DEBUG => ", toliteral(args) + msg));
  endif
endfor
.
```

### New Types
#### Verb Call Handle (TYPE_CALL)
```c
// A simple verb with aliases
@verb $some_thing:"paper rock scissors" this none this
@prog $some_thing:paper
$debug(verb);
.

// Store the calls in a list on $some_thing
@property $some_thing.moves
;$some_thing.moves = {$some_thing::paper, $some_thing::rock, $some_thing::scissors}
=> {$some_thing::paper, $some_thing::rock, $some_thing::scissors}

// Choose handle randomly and call it, five rounds
;;for i in [1..5] $some_thing.moves[random($)]:call(); endfor 

DEBUG => {"rock"} [caller=> $some_thing:rock, line 1]
DEBUG => {"scissors"} [caller=> $some_thing:scissors, line 1]
DEBUG => {"rock"} [caller=> $some_thing:rock, line 1]
DEBUG => {"paper"} [caller=> $some_thing:paper, line 1]
DEBUG => {"paper"} [caller=> $some_thing:paper, line 1]

// Simulate some work
@verb $some_thing:do_work this none this
@prog $some_thing:do_work
{value, callback} = args;
suspend(5);
value = value * 2;
callback:call(value);
.

// Callback verb
@verb $some_thing:cb this none this
@prog $some_thing:cb
$debug(@args);
.

// Test it out!
;$some_thing:do_work(5, $some_thing::cb);
DEBUG => {10} [caller=> $some_thing:cb, line 1]

```
#### Complex Numbers (TYPE_COMPLEX)
```c
;(3 + 4i) + (2 - 1i)
=> (5 + 3i)

;sqrt(-1.0)
=> 1i

;(2.5 + 0.5i) * 3
=> (7.5 + 1.5i)

;(4 + 3i) / (1 - 1i)
=> (0.5 + 3.5i)

;(5 + 1.5i) % 0
=> 5.22015325445528
```
#### New Built-in Functions
```c
  all_contents()    => Recursively get contents of an object
  corified_as()     => Get corified string, i.e. corified_as(#20) => "$string_utils";
  make()            => Fast allocation of lists, makes a list with x copies of y
  range()           => Builtin $list_utils:range(), make list from a range of integers
  shuffle()         => Shuffles a list
  map_args()        => Maps a verb call handle over a list
  intersection()    => Set intersection, fast version of $set_utils:intersection();
  implode()         => Opposite of explode(), joins a list into a string
  str_trim()        => Trim a string
  str_triml()       => Trim left side of a string 
  str_trimr()       => Trim right side of a string
  str_pad()         => Pad a string with characters
  str_padr()        => Pad right side of a string with characters
  str_padl()        => Pad left side of a string with characters
  str_uppercase()   => Convert string to uppercase
  str_lowercase()   => Convert string to lowercase
  str_escape()      => Escape newlines and special characters in string
  str_unescape()    => Unescape newlines and special characters in string
  time_fmt()        => Format strings from timestamps like std::put_time
  time_parse()      => Parse time strings back into timestamps
  parse_ordinal()   => Ordinal ref parsing: parse_ordinal("third jacket") => {3, "jacket"};
  complex_match()   => Advanced object matcher handling ordinal references
  tokenize()        => Process a list of words into tokens for keyword matching 
  tocomplex()       => Convert a value to a complex number
  log2()            => Base-2 logarithm function
  logn()            => Base-n logarithm function
  rand_splitmix64() => Splitmix64 random number generator
  verb_meta()       => Store/retrieve metadata for a verb
  xxhash()          => Fast hash function returning an INT
```
#### Deprecated Functions
```c
  reverse(x)     => x[$..^];
  parent(x)      => `parents(x)[1] ! E_RANGE => $nothing';
  chparent(x, y) => chparents(x, {y});
 ```
 **Note:** The parser fixes these automatically when old DBs are upgraded.

