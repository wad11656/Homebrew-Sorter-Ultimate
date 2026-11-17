SUBDIRS = fs_driver app launcher

all:
	@set -e; for dir in $(SUBDIRS); do $(MAKE) -C $$dir; done

clean:
	@set -e; for dir in $(SUBDIRS); do $(MAKE) clean -C $$dir; done
