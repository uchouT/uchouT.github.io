#include <stdio.h>

typedef struct Logger Logger;

typedef enum {
	INFO,
	ERROR,
} Level;

typedef struct {
	const char *(*name)(Logger *self);
	void (*log)(Logger *self, Level level, const char *msg);
	void (*info)(Logger *self, const char *msg);
	void (*error)(Logger *self, const char *msg);
} LoggerVtable;

/*
 * base class
 */
struct Logger {
	const LoggerVtable *vtable;

	// shared fields here
	const char *prefix;
};

/**
 * dispatch entry
 */
static inline const char *logger_name(Logger *lg)
{
	return lg->vtable->name(lg);
}
static inline void logger_log(Logger *lg, Level level, const char *msg)
{
	lg->vtable->log(lg, level, msg);
}
static inline void logger_info(Logger *lg, const char *msg)
{
	lg->vtable->info(lg, msg);
}
static inline void logger_error(Logger *lg, const char *msg)
{
	lg->vtable->error(lg, msg);
}

/**
 * base class default implementations
 */
static void default_logger_info(Logger *lg, const char *msg)
{
	lg->vtable->log(lg, INFO, msg);
}
static void default_logger_error(Logger *lg, const char *msg)
{
	lg->vtable->log(lg, ERROR, msg);
}

typedef struct {
	Logger base;
	int count;
} SimpleLogger;

static const char *simple_logger_name(Logger *_lg)
{
	return "simple logger";
}

static void simple_logger_log(Logger *lg, Level level, const char *msg)
{
	SimpleLogger *c = (SimpleLogger *)lg;
	c->count += 1;

	const char *level_msg;
	switch (level) {
	case INFO:
		level_msg = "INFO";
		break;
	case ERROR:
		level_msg = "ERROR";
		break;
	}
	printf("%s %s %s: %s\ncounts: %d\n", lg->prefix, logger_name(lg),
	       level_msg, msg, c->count);
}

static const LoggerVtable SIMPLE_LOGGER_VT = {
	.name = simple_logger_name,
	.log = simple_logger_log,

	// inherit from base class default implementations
	.info = default_logger_info,
	.error = default_logger_error
};

SimpleLogger new_simple_logger(const char *prefix)
{
	SimpleLogger lg = { .base = { .vtable = &SIMPLE_LOGGER_VT,
				      .prefix = prefix },
			    .count = 0 };
	return lg;
}

typedef struct {
	Logger base;
} ColorLogger;

static const char *color_logger_name(Logger *_lg)
{
	return "color logger";
}

static void color_logger_log(Logger *lg, Level level, const char *msg)
{
	const char *level_msg;
	switch (level) {
	case INFO:
		level_msg = "INFO";
		break;
	case ERROR:
		level_msg = "ERROR";
		break;
	}
	printf("%s %s %s: %s\n", lg->prefix, logger_name(lg), level_msg, msg);
}

// override
static void color_logger_error(Logger *lg, const char *msg)
{
	printf("%s %s \033[0;31mERROR\033[0m: %s\n", lg->prefix,
	       logger_name(lg), msg);
}

static const LoggerVtable COLOR_LOGGER_VT = {
	.name = color_logger_name,
	.log = color_logger_log,
	.info = default_logger_info,
	.error = color_logger_error,
};

ColorLogger new_color_logger(const char *prefix)
{
	ColorLogger lg = {
		.base = { .vtable = &COLOR_LOGGER_VT, .prefix = prefix },
	};
	return lg;
}

int main()
{
	SimpleLogger sp = new_simple_logger("Nya~");
	ColorLogger cl = new_color_logger("Meow~");
	Logger *logger[] = {
		(Logger *)&sp,
		(Logger *)&cl,
	};

	for (int i = 0; i < 2; ++i) {
		logger_info(logger[i], "starts");
		logger_error(logger[i], "error happens");
	}
}
