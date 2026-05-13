enum Level {
    Info,
    Error,
}

trait Logger {
    fn name(&self) -> &str;
    fn log(&mut self, level: Level, msg: &str);
    fn info(&mut self, msg: &str) {
        self.log(Level::Info, msg);
    }
    fn error(&mut self, msg: &str) {
        self.log(Level::Error, msg);
    }
}

struct SimpleLogger {
    count: u32,
    prefix: &'static str,
}

impl SimpleLogger {
    fn new(prefix: &'static str) -> Self {
        Self { count: 0, prefix }
    }
}

impl Logger for SimpleLogger {
    fn name(&self) -> &str {
        "simple logger"
    }

    fn log(&mut self, level: Level, msg: &str) {
        self.count += 1;
        let level_msg = match level {
            Level::Info => "INFO",
            Level::Error => "ERROR",
        };

        print!(
            "{} {} {level_msg}: {msg}\ncounts: {}\n",
            self.prefix,
            self.name(),
            self.count
        );
    }
}

struct ColorLogger {
    prefix: &'static str,
}

impl ColorLogger {
    fn new(prefix: &'static str) -> Self {
        Self { prefix }
    }
}
impl Logger for ColorLogger {
    fn name(&self) -> &str {
        "color logger"
    }
    fn log(&mut self, level: Level, msg: &str) {
        let level_msg = match level {
            Level::Info => "INFO",
            Level::Error => "ERROR",
        };

        println!("{} {} {level_msg}: {msg}", self.prefix, self.name());
    }
    fn error(&mut self, msg: &str) {
        println!(
            "{} {} \x1b[0;31mERROR\x1b[0m: {msg}",
            self.prefix,
            self.name()
        );
    }
}

fn main() {
    let logger: [&mut dyn Logger; 2] = [
        &mut SimpleLogger::new("Nya~"),
        &mut ColorLogger::new("Meow~"),
    ];
    for lg in logger {
        lg.info("starts");
        lg.error("error happened");
    }
}
