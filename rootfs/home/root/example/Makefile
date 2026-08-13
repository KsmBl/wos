# An example for make.  Try it:
#
#   cd /home/root/example
#   make                 builds report.txt, which is not there yet
#   make                 says there is nothing to do
#   touch greeting.txt   the file it is built from just changed
#   make                 builds it again
#   make clean           removes it
#
# That is the whole idea of make: a target is rebuilt when something it is
# built from has a later modification time than it has, and left alone when
# nothing has changed.  WFS keeps a time for every file, which is what the
# comparison reads.
#
# A recipe line has to start with a real tab, not spaces.

SOURCE = greeting.txt
REPORT = report.txt

# $@ is the target, $< the first thing it is built from.  The recipes here are
# `cat` and `touch` because that is what this machine has: no compiler yet, and
# no way for a command to write its output into a file.  When there is one,
# these lines become the compile and the link and nothing else about the
# Makefile changes.
$(REPORT): $(SOURCE)
	cat $<
	touch $@

show: $(REPORT)
	cat $(REPORT)

clean:
	rm $(REPORT)

.PHONY: show clean
