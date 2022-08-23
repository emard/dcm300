CFLAGS=-Wall -g

project=dcm300
parser=cmdline
version=$(shell ./version.sh)
debrelease=1
architecture=i386

package=$(project)_$(version)-$(debrelease)_$(architecture).deb

OBJECTS=main.o $(project).o $(parser).o
CLIBS=-lusb

GCCOPT=-g -Wall

debianproject=debian/usr/bin/$(project)

# debian parts that can be erased and rebuilt
debianparts=$(debianproject) debian/DEBIAN/control debian/usr/share/doc/$(project)/changelog.gz debian/usr/share/doc/$(project)/README.gz debian/usr/bin/$(project)

# debian/usr/share/doc/vdrsync/copyright debian/usr/share/doc/vdrsync/changelog.gz debian/usr/share/doc/vdrsync/changelog.Debian.gz


all: $(project)

$(parser).c: $(parser).ggo Makefile
	gengetopt < $< --file-name=$(parser) # --unamed-opts

$(parser).h: $(parser).ggo Makefile
	gengetopt < $< --file-name=$(parser) # --unamed-opts

$(parser).o: $(parser).c $(parser).h Makefile
	gcc -c $(CFLAGS) $(parser).c

$(project).o: $(project).c $(project).h Makefile
	gcc -c $(CFLAGS) $(project).c

main.o: main.c $(project).h $(parser).h Makefile
	gcc -c $(CFLAGS) main.c -o $@

$(project): $(OBJECTS) Makefile
	gcc $(CFLAGS) $(OBJECTS) $(CLIBS) -o $@

$(debianproject): $(project)
	strip $< -o $@
	chmod og+rx $@

debian/usr/share/doc/$(project)/changelog.gz: changelog
	gzip -9 < $< > $@
	chmod og+r $@

debian/usr/share/doc/$(project)/README.gz: README
	gzip -9 < $< > $@
	chmod og+r $@
	                

debian/DEBIAN/control: control cmdline.ggo
	sed -e "s/VERSION/$(version)-$(debrelease)/" control > $@

$(package): $(debianparts)
	find debian -name "*~" -exec rm -f {} \;
	fakeroot dpkg-deb --build debian $(package)

package: $(package)
	lintian $<

run: $(project)
	./$< -d ../images/dcm300.raw > /tmp/image.pnm

debug: $(project)
	gdb -x cmd.gdb ./$<

clean:
	rm -f $(package) $(project) $(OBJECTS) $(parser).o $(parser).c $(parser).h $(debianparts) DEADJOE *~
