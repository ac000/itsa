# Coding Style

We follow the Linux kernel [coding style](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/Documentation/process/coding-style.rst).

As a general rule :-

- all indentation is tabs (set to 8 char) with the exception of
  continuation lines that are aligned with tabs and then spaces

- all keywords followed by a '(' have a space in between

```C
	if (condition)

	for (i = 0; i < 5; i++)
```

- function calls do NOT have a space between their name and argument

```C
	i = some_function(argument);
```

- usually there is no space on the inside of parenthesis (see examples
  above)

- function / method implementations have their opening curly braces in
  column 1

- all other opening curly braces follow at the end of the line, with a
  space separating them:

```C
	if (condition) {
		dosomething();
		dosomethingelse();
	}
```

- both sides of an if / else clause either use or do not use curly braces:

```C
	if (condition)
		i = 4;
	else
		j = 6;

	if (condition) {
		i = 6;
	} else {
		i = 4;
		j = 6;
	}
```

- don't do assignments inside if () statements, always do it this way

```C
	res = something();
	if (!res)
		return NULL;
```

- if you don't need the result you can do

```C
	if (!something())
		return NULL;
```

- use space to make visual separation easier

```C
	a = b + 3 + e / 4;
```

- continuation lines have the operator / comma at the end

```C
	if (very_long_conditiont_1 ||
	    condition_2)

        b = a + (c + d +
                 f + z);
```

- switch statements with blocks are a little bit special (to avoid indenting
  too far)

```C
	switch (foo) {
	case FIRST:
		whatever();
		break;
	case SECOND: {
		int i;
		for (i = 0; i < 5; i++)
			do_something(i);
	}
	}
```

- comments should be C style not C++/C99

  for single line comments

```C
	/* This is a single line comment */
```

  for multi-line comments

```C
	/*
	 * This is a multi
	 * line comment
	 */
```

- variable declarations should be at the beginning of a code block, not
  interspersed in the middle

- variable and function naming should be all lower case with _ used for spaces
