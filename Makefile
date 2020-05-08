CXX=		g++ $(CCFLAGS)
MAPCOPY=	mapcopy.o
OBJS =		$(MAPCOPY)

LIBS=		-pthread

CCFLAGS= -Wall -Wextra -O3

all:		mapcopy

mapcopy:	$(MAPCOPY)
			$(CXX) -o mapcopy $(MAPCOPY) $(LIBS)


clean:
			rm -f $(OBJS) mapcopy

