/* Copyright (c) 1998 Silicon Graphics, Inc. */

#ifndef lint
static char SCCSid[] = "$SunId$ SGI";
#endif

/*
 * OpenGL driver for holodeck display.
 * Based on GLX driver using T-mesh.
 *
 * Define symbol STEREO for stereo viewing.
 * Define symbol DOBJ for display object viewing.
 */

#ifdef NOSTEREO
#ifdef STEREO
#undef STEREO
#else
#undef NOSTEREO
#endif
#endif

#include "standard.h"

#include <sys/types.h>
#include <GL/glx.h>
#include <GL/glu.h>
#ifdef STEREO
#include <X11/extensions/SGIStereo.h>
#endif

#include "rhd_odraw.h"
#ifdef DOBJ
#include "rhdobj.h"
#endif

#include "x11icon.h"

#ifndef RAYQLEN
#define RAYQLEN		50000		/* max. rays to queue before flush */
#endif

#ifndef PORTALP
#define PORTRED		2		/* portal red color */
#define PORTGRN		-1		/* portal green left alone */
#define PORTBLU		128		/* portal blue color */
#define PORTALP		-1		/* don't use alpha channel */
#endif
#define isportal(c)	((PORTRED<0 || (c)[0]==PORTRED) && \
				(PORTGRN<0 || (c)[1]==PORTGRN) && \
				(PORTBLU<0 || (c)[2]==PORTBLU) && \
				(PORTALP<0 || (c)[3]==PORTALP))

#ifndef FEQ
#define FEQ(a,b)	((a)-(b) <= FTINY && (a)-(b) >= -FTINY)
#endif

#define GAMMA		1.4		/* default gamma correction */

#define FRAMESTATE(s)	(((s)&(ShiftMask|ControlMask))==(ShiftMask|ControlMask))

#define MOVPCT		7		/* percent distance to move /frame */
#define MOVDIR(b)	((b)==Button1 ? 1 : (b)==Button2 ? 0 : -1)
#define MOVDEG		(-5)		/* degrees to orbit CW/down /frame */
#define MOVORB(s)	((s)&ShiftMask ? 1 : (s)&ControlMask ? -1 : 0)

#define MINWIDTH	480		/* minimum graphics window width */
#define MINHEIGHT	400		/* minimum graphics window height */

#define VIEWDIST	356		/* assumed viewing distance (mm) */

#define BORWIDTH	5		/* border width */

#define setstereobuf(bid)	(glXWaitGL(), \
				XSGISetStereoBuffer(ourdisplay, gwind, bid), \
				glXWaitX())

#define  ourscreen	DefaultScreen(ourdisplay)
#define  ourroot	RootWindow(ourdisplay,ourscreen)
#define  ourmask	(StructureNotifyMask|ExposureMask|KeyPressMask|\
			ButtonPressMask|ButtonReleaseMask)

#define  levptr(etype)	((etype *)&currentevent)

struct driver	odev;			/* global device driver structure */

char odev_args[64];			/* command arguments */

static GLfloat	*depthbuffer = NULL;	/* depth buffer */

#ifdef STEREO
static VIEW	vwright;		/* right eye view */
static GLfloat	*depthright = NULL;	/* right depth buffer */
#endif

static int	rayqleft = 0;		/* rays left to queue before flush */

static XEvent  currentevent;		/* current event */

static int  mapped = 0;			/* window is mapped? */
static unsigned long  ourblack=0, ourwhite=~0;

static Display  *ourdisplay = NULL;	/* our display */
static XVisualInfo  *ourvinf;		/* our visual information */
static Window  gwind = 0;		/* our graphics window */
static GLXContext	gctx;		/* our GLX context */

static double	pwidth, pheight;	/* pixel dimensions (mm) */

static double	dev_zmin, dev_zmax;	/* fore and aft clipping plane dist. */
static double	dev_zrat;		/* (1. - dev_zmin/dev_zmax) */

#define setzrat()	(dev_zrat = 1. - dev_zmin/dev_zmax)
#define mapdepth(d)	((d)>0.9995 ? FHUGE : dev_zmin/(1.-(d)*dev_zrat))

static int	inpresflags;		/* input result flags */

static int	headlocked;		/* lock vertical motion */

static int	isperspective;		/* perspective/ortho view */

static int	viewsteady;		/* is view steady? */

static int  resizewindow(), getevent(), getkey(), moveview(), wipeclean(),
		xferdepth(), freedepth(), setglortho(),
		setglpersp(), getframe(), getmove(), fixwindow(), mytmflags();

static double	getdistance();

#ifdef STEREO
static int  pushright(), popright();
#endif

extern int	gmPortals;	/* GL portal list id */

extern time_t	time();


dev_open(id)			/* initialize GLX driver */
char  *id;
{
	extern char	*getenv();
	static RGBPRIMS	myprims = STDPRIMS;
#if (PORTALP<0)
	static int	atlBest[] = {GLX_RGBA, GLX_DOUBLEBUFFER,
				GLX_RED_SIZE,8, GLX_GREEN_SIZE,8,
				GLX_BLUE_SIZE,8, GLX_DEPTH_SIZE,15, None};
	static int	atlOK[] = {GLX_RGBA, GLX_DOUBLEBUFFER,
				GLX_RED_SIZE,4, GLX_GREEN_SIZE,4,
				GLX_BLUE_SIZE,4, GLX_DEPTH_SIZE,15, None};
#else
	static int	atlBest[] = {GLX_RGBA, GLX_DOUBLEBUFFER,
				GLX_RED_SIZE,8, GLX_GREEN_SIZE,8,
				GLX_BLUE_SIZE,8, GLX_ALPHA_SIZE,2,
				GLX_DEPTH_SIZE,15, None};
	static int	atlOK[] = {GLX_RGBA, GLX_DOUBLEBUFFER,
				GLX_RED_SIZE,4, GLX_GREEN_SIZE,4,
				GLX_BLUE_SIZE,4, GLX_ALPHA_SIZE,2,
				GLX_DEPTH_SIZE,15, None};
#endif
	char	*ev;
	double	gamval = GAMMA;
	RGBPRIMP	dpri = stdprims;
	XSetWindowAttributes	ourwinattr;
	XWMHints	ourxwmhints;
	XSizeHints	oursizhints;
					/* check for unsupported stereo */
#ifdef NOSTEREO
	error(INTERNAL, "stereo display driver unavailable");
#endif
					/* open display server */
	ourdisplay = XOpenDisplay(NULL);
	CHECK(ourdisplay==NULL, USER,
			"cannot open X-windows; DISPLAY variable set?");
#ifdef STEREO
	switch (XSGIQueryStereoMode(ourdisplay, ourroot)) {
	case STEREO_TOP:
	case STEREO_BOTTOM:
		break;
	case STEREO_OFF:
		error(USER,
		"wrong video mode: run \"/usr/gfx/setmon -n STR_TOP\" first");
	case X_STEREO_UNSUPPORTED:
		error(USER, "stereo mode not supported on this screen");
	default:
		error(INTERNAL, "unknown stereo mode");
	}
#endif
					/* find a usable visual */
	ourvinf = glXChooseVisual(ourdisplay, ourscreen, atlBest);
	if (ourvinf == NULL)
		ourvinf = glXChooseVisual(ourdisplay, ourscreen, atlOK);
	CHECK(ourvinf==NULL, USER, "no suitable visuals available");
					/* get a context */
	gctx = glXCreateContext(ourdisplay, ourvinf, NULL, GL_TRUE);
					/* set gamma and tone mapping */
	if ((ev = XGetDefault(ourdisplay, "radiance", "gamma")) != NULL
			|| (ev = getenv("DISPLAY_GAMMA")) != NULL)
		gamval = atof(ev);
	if ((ev = getenv("DISPLAY_PRIMARIES")) != NULL &&
			sscanf(ev, "%f %f %f %f %f %f %f %f",
				&myprims[RED][CIEX],&myprims[RED][CIEY],
				&myprims[GRN][CIEX],&myprims[GRN][CIEY],
				&myprims[BLU][CIEX],&myprims[BLU][CIEY],
				&myprims[WHT][CIEX],&myprims[WHT][CIEY]) >= 6)
		dpri = myprims;
	if (tmInit(mytmflags(), dpri, gamval) == NULL)
		error(SYSTEM, "not enough memory in dev_open");
					/* open window */
	ourwinattr.background_pixel = ourblack;
	ourwinattr.border_pixel = ourblack;
	ourwinattr.event_mask = ourmask;
					/* this is stupid */
	ourwinattr.colormap = XCreateColormap(ourdisplay, ourroot,
				ourvinf->visual, AllocNone);
	gwind = XCreateWindow(ourdisplay, ourroot, 0, 0,
		DisplayWidth(ourdisplay,ourscreen)-2*BORWIDTH,
#ifdef STEREO
		(DisplayHeight(ourdisplay,ourscreen)-2*BORWIDTH)/2,
#else
		DisplayHeight(ourdisplay,ourscreen)-2*BORWIDTH,
#endif
		BORWIDTH, ourvinf->depth, InputOutput, ourvinf->visual,
		CWBackPixel|CWBorderPixel|CWColormap|CWEventMask, &ourwinattr);
	CHECK(gwind==0, SYSTEM, "cannot create window");
   	XStoreName(ourdisplay, gwind, id);
					/* set window manager hints */
	ourxwmhints.flags = InputHint|IconPixmapHint;
	ourxwmhints.input = True;
	ourxwmhints.icon_pixmap = XCreateBitmapFromData(ourdisplay,
			gwind, x11icon_bits, x11icon_width, x11icon_height);
	XSetWMHints(ourdisplay, gwind, &ourxwmhints);
	oursizhints.min_width = MINWIDTH;
#ifdef STEREO
	oursizhints.min_height = MINHEIGHT/2;
	oursizhints.max_width = DisplayWidth(ourdisplay,ourscreen)-2*BORWIDTH;
	oursizhints.max_height = (DisplayHeight(ourdisplay,ourscreen) -
					2*BORWIDTH)/2;
	oursizhints.flags = PMinSize|PMaxSize;
#else
	oursizhints.min_height = MINHEIGHT;
	oursizhints.flags = PMinSize;
#endif
	XSetNormalHints(ourdisplay, gwind, &oursizhints);
					/* set GLX context */
	glXMakeCurrent(ourdisplay, gwind, gctx);
	glEnable(GL_DEPTH_TEST);
	glDepthFunc(GL_LEQUAL);
	glEnable(GL_DITHER);
	glFrontFace(GL_CCW);
	glDisable(GL_CULL_FACE);
	glLightModeli(GL_LIGHT_MODEL_TWO_SIDE, GL_TRUE);
	glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
					/* figure out sensible view */
	pwidth = (double)DisplayWidthMM(ourdisplay, ourscreen) /
			DisplayWidth(ourdisplay, ourscreen);
	pheight = (double)DisplayHeightMM(ourdisplay, ourscreen) /
			DisplayHeight(ourdisplay, ourscreen);
#ifdef STEREO
	pheight *= 2.;
	setstereobuf(STEREO_BUFFER_LEFT);
#endif
	checkglerr("setting rendering parameters");
	copystruct(&odev.v, &stdview);
	odev.v.type = VT_PER;
	headlocked = 0;			/* free up head movement */
	viewsteady = 1;			/* view starts static */
	isperspective = -1;		/* but no view set, yet */
					/* map the window */
	XMapWindow(ourdisplay, gwind);
	dev_input();			/* sets size and view angles */
	if (!odInit(DisplayWidth(ourdisplay,ourscreen) *
			DisplayHeight(ourdisplay,ourscreen) / 4))
		error(SYSTEM, "insufficient memory for value storage");
	odev.name = id;
	odev.firstuse = 1;		/* can't recycle samples */
	odev.ifd = ConnectionNumber(ourdisplay);
}


dev_close()			/* close our display and free resources */
{
#ifdef DOBJ
	dobj_cleanup();
#endif
	freedepth();
	gmEndGeom();
	gmEndPortal();
	odDone();
	glXMakeCurrent(ourdisplay, None, NULL);
	glXDestroyContext(ourdisplay, gctx);
	XDestroyWindow(ourdisplay, gwind);
	gwind = 0;
	XCloseDisplay(ourdisplay);
	ourdisplay = NULL;
	tmDone(NULL);
	odev.v.type = 0;
	odev.hres = odev.vres = 0;
	odev.ifd = -1;
}


dev_clear()			/* clear our representation */
{
	wipeclean();
	rayqleft = 0;			/* hold off update */
}


int
dev_view(nv)			/* assign new driver view */
register VIEW	*nv;
{
	double	d;

	if (nv->type != VT_PER ||		/* check view legality */
			nv->horiz > 160. || nv->vert > 160.) {
		error(COMMAND, "illegal view type/angle");
		nv->type = odev.v.type;
		nv->horiz = odev.v.horiz;
		nv->vert = odev.v.vert;
		return(0);
	}
	if (nv != &odev.v) {
						/* resize window? */
		if (!FEQ(nv->horiz,odev.v.horiz) ||
				!FEQ(nv->vert,odev.v.vert)) {
			int	dw = DisplayWidth(ourdisplay,ourscreen);
			int	dh = DisplayHeight(ourdisplay,ourscreen);

			dw -= 25;	/* for window frame */
			dh -= 50;
#ifdef STEREO
			dh /= 2;
#endif
			odev.hres = 2.*VIEWDIST/pwidth *
					tan(PI/180./2.*nv->horiz);
			odev.vres = 2.*VIEWDIST/pheight *
					tan(PI/180./2.*nv->vert);
			if (odev.hres > dw) {
				odev.vres = dw * odev.vres / odev.hres;
				odev.hres = dw;
			}
			if (odev.vres > dh) {
				odev.hres = dh * odev.hres / odev.vres;
				odev.vres = dh;
			}
			XResizeWindow(ourdisplay, gwind, odev.hres, odev.vres);
			dev_input();	/* get resize event */
		}
		copystruct(&odev.v, nv);	/* setview() already called */
#ifdef STEREO
		copystruct(&vwright, nv);
		d = eyesepdist / sqrt(nv->hn2);
		VSUM(vwright.vp, nv->vp, nv->hvec, d);
		/* setview(&vwright);	-- Unnecessary */
#endif
	}
	wipeclean();
	return(1);
}


dev_section(gfn, pfn)		/* add octree for geometry rendering */
char	*gfn, *pfn;
{
	extern char	*index();
	char	*cp;

	if (gfn == NULL) {
		gmEndGeom();
		gmEndPortal();
		wipeclean();		/* new geometry, so redraw it */
		return;
	}
	if (access(gfn, R_OK) == 0)
		gmNewGeom(gfn);
#ifdef DEBUG
	else {
		sprintf(errmsg, "cannot load octree \"%s\"", gfn);
		error(WARNING, errmsg);
	}
#endif
	if (pfn != NULL)
		gmNewPortal(pfn);
}


dev_auxcom(cmd, args)		/* process an auxiliary command */
char	*cmd, *args;
{
#ifdef DOBJ
	if (dobj_command(cmd, args) >= 0)
		return;
#endif
	sprintf(errmsg, "%s: unknown command", cmd);
	error(COMMAND, errmsg);
}


VIEW *
dev_auxview(n, hvres)		/* return nth auxiliary view */
int	n;
int	hvres[2];
{
	hvres[0] = odev.hres; hvres[1] = odev.vres;
	if (n == 0)
		return(&odev.v);
#ifdef STEREO
	if (n == 1)
		return(&vwright);
#endif
	return(NULL);
}


int
dev_input()			/* get X11 input */
{
	inpresflags = 0;

	do
		getevent();

	while (XPending(ourdisplay) > 0);

	odev.inpready = 0;

	return(inpresflags);
}


dev_value(c, d, p)		/* add a pixel value to our texture */
COLR	c;
FVECT	d, p;
{
#ifdef DOBJ
	if (dobj_lightsamp != NULL) {	/* in light source sampling */
		(*dobj_lightsamp)(c, d, p);
		return;
	}
#endif
	odSample(c, d, p);		/* add to display representation */
	if (!--rayqleft)
		dev_flush();		/* flush output */
}


int
dev_flush()			/* flush output as appropriate */
{
	int	ndrawn;

	if (mapped && isperspective > 0) {
#ifdef STEREO
		pushright();			/* draw right eye */
		ndrawn = gmDrawGeom();
#ifdef DOBJ
		ndrawn += dobj_render();
#endif
		if (ndrawn && gmPortals)
			gmDrawPortals(PORTRED,PORTGRN,PORTBLU,PORTALP);
		checkglerr("rendering right eye");
		popright();			/* draw left eye */
#endif
		ndrawn = gmDrawGeom();
#ifdef DOBJ
		ndrawn += dobj_render();
#endif
		if (ndrawn && gmPortals)
			gmDrawPortals(PORTRED,PORTGRN,PORTBLU,PORTALP);
		glXSwapBuffers(ourdisplay, gwind);
		checkglerr("rendering base view");
	}
	if (mapped && viewsteady)
		if (isperspective > 0) {	/* first time after steady */
			if (ndrawn)
				xferdepth();	/* transfer and clear depth */
			setglortho();		/* set orthographic view */
		} else if (!isperspective) {
#ifdef STEREO
			pushright();
			odUpdate(1);		/* draw right eye */
			popright();
#endif
			odUpdate(0);		/* draw left eye */
			glFlush();		/* flush OpenGL */
		}
	rayqleft = RAYQLEN;
					/* flush X11 and return # pending */
	return(odev.inpready = XPending(ourdisplay));
}


checkglerr(where)		/* check for GL or GLU error */
char	*where;
{
	register GLenum	errcode;

	while ((errcode = glGetError()) != GL_NO_ERROR) {
		sprintf(errmsg, "OpenGL error %s: %s",
				where, gluErrorString(errcode));
		error(WARNING, errmsg);
	}
}


static
xferdepth()			/* load and clear depth buffer */
{
	register GLfloat	*dbp;
	register GLubyte	*cbuf;

	if (depthbuffer == NULL) {	/* allocate private depth buffer */
#ifdef STEREO
		depthright = (GLfloat *)malloc(
				odev.hres*odev.vres*sizeof(GLfloat));
#endif
		depthbuffer = (GLfloat *)malloc(
				odev.hres*odev.vres*sizeof(GLfloat));
		CHECK(depthbuffer==NULL, SYSTEM, "out of memory in xferdepth");
	}
				/* allocate alpha buffer for portals */
	if (gmPortals)
		cbuf = (GLubyte *)malloc(odev.hres*odev.vres *
							(4*sizeof(GLubyte)));
	else
		cbuf = NULL;
#ifdef STEREO
	setstereobuf(STEREO_BUFFER_RIGHT);
	glReadPixels(0, 0, odev.hres, odev.vres,
			GL_DEPTH_COMPONENT, GL_FLOAT, depthright);
	if (cbuf != NULL)
		glReadPixels(0, 0, odev.hres, odev.vres,
				GL_RGBA, GL_UNSIGNED_BYTE, cbuf);
	for (dbp = depthright + odev.hres*odev.vres; dbp-- > depthright; )
		if (cbuf != NULL && isportal(cbuf+4*(dbp-depthright)))
			*dbp = FHUGE;
		else
			*dbp = mapdepth(*dbp);
	glClear(GL_DEPTH_BUFFER_BIT);
	setstereobuf(STEREO_BUFFER_LEFT);
	odDepthMap(1, depthright);
#endif
				/* read back depth buffer */
	glReadPixels(0, 0, odev.hres, odev.vres,
			GL_DEPTH_COMPONENT, GL_FLOAT, depthbuffer);
	if (cbuf != NULL)
		glReadPixels(0, 0, odev.hres, odev.vres,
				GL_RGBA, GL_UNSIGNED_BYTE, cbuf);
	for (dbp = depthbuffer + odev.hres*odev.vres; dbp-- > depthbuffer; )
		if (cbuf != NULL && isportal(cbuf+4*(dbp-depthbuffer)))
			*dbp = FHUGE;
		else
			*dbp = mapdepth(*dbp);
	glClear(GL_DEPTH_BUFFER_BIT);		/* clear system depth buffer */
	if (cbuf != NULL)
		free((char *)cbuf);		/* free our color buffer */
	odDepthMap(0, depthbuffer);		/* transfer depth data */
}


static
freedepth()				/* free recorded depth buffer */
{
	if (depthbuffer == NULL)
		return;
#ifdef STEREO
	odDepthMap(1, NULL);
	free((char *)depthright);
	depthright = NULL;
#endif
	odDepthMap(0, NULL);
	free((char *)depthbuffer);
	depthbuffer = NULL;
}


static double
getdistance(dx, dy, direc)	/* distance from fore plane along view ray */
int	dx, dy;
FVECT	direc;
{
	GLfloat	gldepth;
	GLubyte	glcolor[4];
	double	dist;

	if (dx<0 | dx>=odev.hres | dy<0 | dy>=odev.vres)
		return(FHUGE);
	if (depthbuffer != NULL)
		dist = depthbuffer[dy*odev.hres + dx];
	else {
		glReadPixels(dx,dy, 1,1, GL_DEPTH_COMPONENT,
				GL_FLOAT, &gldepth);
		if (gmPortals) {
			glReadPixels(dx,dy, 1,1, GL_RGBA,
					GL_UNSIGNED_BYTE, glcolor);
			if (isportal(glcolor))
				return(FHUGE);
		}
		dist = mapdepth(gldepth);
	}
	if (dist >= .99*FHUGE)
		return(FHUGE);
	return((dist-odev.v.vfore)/DOT(direc,odev.v.vdir));
}


#ifdef STEREO
static
pushright()			/* push on right view & buffer */
{
	double	d;

	setstereobuf(STEREO_BUFFER_RIGHT);
	if (isperspective > 0) {
		glMatrixMode(GL_MODELVIEW);
		glPushMatrix();
		d = -eyesepdist / sqrt(odev.v.hn2);
		glTranslated(d*odev.v.hvec[0], d*odev.v.hvec[1],
				d*odev.v.hvec[2]);
		checkglerr("setting right view");
	}
}


static
popright()			/* pop off right view & buffer */
{
	if (isperspective > 0) {
		glMatrixMode(GL_MODELVIEW);
		glPopMatrix();
	}
	setstereobuf(STEREO_BUFFER_LEFT);
}
#endif


static int
mytmflags()			/* figure out tone mapping flags */
{
	extern char	*progname;
	register char	*cp, *tail;
					/* find basic name */
	for (cp = tail = progname; *cp; cp++)
		if (*cp == '/')
			tail = cp+1;
	for (cp = tail; *cp && *cp != '.'; cp++)
		;
#ifdef DEBUG
	if (cp > tail && cp[-1] == 'h')
		return(TM_F_HUMAN);
	else
		return(TM_F_CAMERA);
#else
	if (cp > tail && cp[-1] == 'h')
		return(TM_F_HUMAN|TM_F_NOSTDERR);
	else
		return(TM_F_CAMERA|TM_F_NOSTDERR);
#endif
}


static
getevent()			/* get next event */
{
	XNextEvent(ourdisplay, levptr(XEvent));
	switch (levptr(XEvent)->type) {
	case ConfigureNotify:
		resizewindow(levptr(XConfigureEvent));
		break;
	case UnmapNotify:
		mapped = 0;
		break;
	case MapNotify:
		odRemap(0);
		mapped = 1;
		break;
	case Expose:
		fixwindow(levptr(XExposeEvent));
		break;
	case KeyPress:
		getkey(levptr(XKeyPressedEvent));
		break;
	case ButtonPress:
 		if (FRAMESTATE(levptr(XButtonPressedEvent)->state))
 			getframe(levptr(XButtonPressedEvent));
 		else
 			getmove(levptr(XButtonPressedEvent));
		break;
	}
}


static
draw3dline(wp)			/* draw 3d line in world coordinates */
register FVECT	wp[2];
{
	glVertex3d(wp[0][0], wp[0][1], wp[0][2]);
	glVertex3d(wp[1][0], wp[1][1], wp[1][2]);
}


static
draw_grids(fore)		/* draw holodeck section grids */
int	fore;
{
	glPushAttrib(GL_LIGHTING_BIT|GL_ENABLE_BIT);
	glDisable(GL_LIGHTING);
	if (fore)
		glColor3ub(0, 255, 255);
	else
		glColor3ub(0, 0, 0);
	glBegin(GL_LINES);		/* draw each grid line */
	gridlines(draw3dline);
	glEnd();
	checkglerr("drawing grid lines");
	glPopAttrib();
}


static
moveview(dx, dy, mov, orb)	/* move our view */
int	dx, dy, mov, orb;
{
	VIEW	nv;
	FVECT	odir, v1, wip;
	double	d, d1;
	register int	li;
				/* start with old view */
	copystruct(&nv, &odev.v);
				/* orient our motion */
	if (viewray(v1, odir, &odev.v,
			(dx+.5)/odev.hres, (dy+.5)/odev.vres) < -FTINY)
		return(0);		/* outside view */
	if (mov | orb) {	/* moving relative to geometry */
		d = getdistance(dx, dy, odir);	/* distance from front plane */
#ifdef DOBJ
		d1 = dobj_trace(NULL, v1, odir);
		if (d1 < d)
			d = d1;
#endif
		if (d >= .99*FHUGE)
			d = 0.5*(dev_zmax+dev_zmin);	/* just guess */
		VSUM(wip, v1, odir, d);
		VSUB(odir, wip, odev.v.vp);
	} else			/* panning with constant viewpoint */
		VCOPY(nv.vdir, odir);
	if (orb && mov) {		/* orbit left/right */
		spinvector(odir, odir, nv.vup, d=MOVDEG*PI/180.*mov);
		VSUM(nv.vp, wip, odir, -1.);
		spinvector(nv.vdir, nv.vdir, nv.vup, d);
	} else if (orb) {		/* orbit up/down */
		fcross(v1, odir, nv.vup);
		if (normalize(v1) == 0.)
			return(0);
		spinvector(odir, odir, v1, d=MOVDEG*PI/180.*orb);
		VSUM(nv.vp, wip, odir, -1.);
		spinvector(nv.vdir, nv.vdir, v1, d);
	} else if (mov) {		/* move forward/backward */
		d = MOVPCT/100. * mov;
		VSUM(nv.vp, nv.vp, odir, d);
	}
	if (!mov ^ !orb && headlocked) {	/* restore head height */
		VSUM(v1, odev.v.vp, nv.vp, -1.);
		d = DOT(v1, nv.vup);
		VSUM(nv.vp, nv.vp, odev.v.vup, d);
	}
	if (setview(&nv) != NULL)
		return(0);	/* illegal view */
	dev_view(&nv);
	inpresflags |= DFL(DC_SETVIEW);
	return(1);
}


static
getframe(ebut)				/* get focus frame */
XButtonPressedEvent	*ebut;
{
	int	startx = ebut->x, starty = ebut->y;
	int	endx, endy;

	XMaskEvent(ourdisplay, ButtonReleaseMask, levptr(XEvent));
	endx = levptr(XButtonReleasedEvent)->x;
	endy = levptr(XButtonReleasedEvent)->y;
	if (endx == startx | endy == starty) {
		XBell(ourdisplay, 0);
		return;
	}
	if (endx < startx) {register int c = endx; endx = startx; startx = c;}
	if (endy < starty) {register int c = endy; endy = starty; starty = c;}
	sprintf(odev_args, "%.3f %.3f %.3f %.3f",
			(startx+.5)/odev.hres, 1.-(endy+.5)/odev.vres,
			(endx+.5)/odev.hres, 1.-(starty+.5)/odev.vres);
	inpresflags |= DFL(DC_FOCUS);
}


static
getmove(ebut)				/* get view change */
XButtonPressedEvent	*ebut;
{
	int	movdir = MOVDIR(ebut->button);
	int	movorb = MOVORB(ebut->state);
	int	ndrawn;
	Window	rootw, childw;
	int	rootx, rooty, wx, wy;
	unsigned int	statemask;

	XNoOp(ourdisplay);		/* makes sure we're not idle */

	viewsteady = 0;			/* flag moving view */
	setglpersp(&odev.v);		/* start us off in perspective */
	while (!XCheckMaskEvent(ourdisplay,
			ButtonReleaseMask, levptr(XEvent))) {
					/* get cursor position */
		if (!XQueryPointer(ourdisplay, gwind, &rootw, &childw,
				&rootx, &rooty, &wx, &wy, &statemask))
			break;		/* on another screen */
					/* compute view motion */
		if (!moveview(wx, odev.vres-1-wy, movdir, movorb)) {
			sleep(1);
			continue;	/* cursor in bad place */
		}
		draw_grids(1);		/* redraw grid */
#ifdef STEREO
		pushright();
		draw_grids(1);
		ndrawn = gmDrawGeom();
#ifdef DOBJ
		ndrawn += dobj_render();
#endif
		if (ndrawn && gmPortals)
			gmDrawPortals(PORTRED,PORTGRN,PORTBLU,PORTALP);
		popright();
#endif
					/* redraw octrees */
		ndrawn = gmDrawGeom();
#ifdef DOBJ
		ndrawn += dobj_render();	/* redraw objects */
#endif
		if (ndrawn && gmPortals)
			gmDrawPortals(PORTRED, PORTGRN, PORTBLU, PORTALP);
		glXSwapBuffers(ourdisplay, gwind);
		if (!ndrawn)
			sleep(1);	/* for reasonable interaction */
	}
	if (!(inpresflags & DFL(DC_SETVIEW))) {	/* do final motion */
		movdir = MOVDIR(levptr(XButtonReleasedEvent)->button);
		wx = levptr(XButtonReleasedEvent)->x;
		wy = levptr(XButtonReleasedEvent)->y;
		moveview(wx, odev.vres-1-wy, movdir, movorb);
	}
	viewsteady = 1;			/* done goofing around */
}


static
setglpersp(vp)			/* set perspective view in GL */
register VIEW	*vp;
{
	double	d, xmin, xmax, ymin, ymax;
	GLfloat	vec[4];
	double	depthlim[2];
					/* set depth limits */
	gmDepthLimit(depthlim, odev.v.vp, odev.v.vdir);
	if (depthlim[0] >= depthlim[1]) {
		dev_zmin = 1.;
		dev_zmax = 100.;
	} else {
		dev_zmin = 0.5*depthlim[0];
		dev_zmax = 1.75*depthlim[1];
		if (dev_zmin > dev_zmax/5.)
			dev_zmin = dev_zmax/5.;
	}
	if (odev.v.vfore > FTINY)
		dev_zmin = odev.v.vfore;
	if (odev.v.vaft > FTINY)
		dev_zmax = odev.v.vaft;
	if (dev_zmin < dev_zmax/100.)
		dev_zmin = dev_zmax/100.;
	setzrat();
	xmax = dev_zmin * tan(PI/180./2. * odev.v.horiz);
	xmin = -xmax;
	d = odev.v.hoff * (xmax - xmin);
	xmin += d; xmax += d;
	ymax = dev_zmin * tan(PI/180./2. * odev.v.vert);
	ymin = -ymax;
	d = odev.v.voff * (ymax - ymin);
	ymin += d; ymax += d;
					/* set view matrix */
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glFrustum(xmin, xmax, ymin, ymax, dev_zmin, dev_zmax);
	gluLookAt(odev.v.vp[0], odev.v.vp[1], odev.v.vp[2],
		odev.v.vp[0] + odev.v.vdir[0],
		odev.v.vp[1] + odev.v.vdir[1],
		odev.v.vp[2] + odev.v.vdir[2],
		odev.v.vup[0], odev.v.vup[1], odev.v.vup[2]);
	checkglerr("setting perspective view");
	isperspective = 1;
	vec[0] = vec[1] = vec[2] = 0.; vec[3] = 1.;
	glLightModelfv(GL_LIGHT_MODEL_AMBIENT, vec);
	vec[0] = -odev.v.vdir[0];
	vec[1] = -odev.v.vdir[1];
	vec[2] = -odev.v.vdir[2];
	vec[3] = 0.;
	glLightfv(GL_LIGHT0, GL_POSITION, vec);
	vec[0] = vec[1] = vec[2] = .7; vec[3] = 1.;
	glLightfv(GL_LIGHT0, GL_SPECULAR, vec);
	glLightfv(GL_LIGHT0, GL_DIFFUSE, vec);
	vec[0] = vec[1] = vec[2] = .3; vec[3] = 1.;
	glLightfv(GL_LIGHT0, GL_AMBIENT, vec);
	glEnable(GL_LIGHT0);
	glEnable(GL_LIGHTING);		/* light our GL objects */
	glShadeModel(GL_SMOOTH);
}


static
setglortho()			/* set up orthographic view for cone drawing */
{
	glDrawBuffer(GL_FRONT);		/* use single-buffer mode */
					/* set view matrix */
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glOrtho(0., (double)odev.hres, 0., (double)odev.vres,
			0.001*OMAXDEPTH, 1.001*(-OMAXDEPTH));
	checkglerr("setting orthographic view");
	isperspective = 0;
	glDisable(GL_LIGHTING);		/* cones are constant color */
	glShadeModel(GL_FLAT);
}


static
wipeclean()			/* prepare for redraw */
{
	glDrawBuffer(GL_BACK);		/* use double-buffer mode */
					/* clear buffers */
#ifdef STEREO
	setstereobuf(STEREO_BUFFER_RIGHT);
	glClear(GL_DEPTH_BUFFER_BIT|GL_COLOR_BUFFER_BIT);
	setstereobuf(STEREO_BUFFER_LEFT);
#endif
	glClear(GL_DEPTH_BUFFER_BIT|GL_COLOR_BUFFER_BIT);
	freedepth();
	if (viewsteady)			/* clear samples if steady */
		odClean();
	setglpersp(&odev.v);		/* reset view & clipping planes */
}


static
getkey(ekey)				/* get input key */
register XKeyPressedEvent  *ekey;
{
	Window	rootw, childw;
	int	rootx, rooty, wx, wy;
	unsigned int	statemask;
	int  n;
	char	buf[8];

	n = XLookupString(ekey, buf, sizeof(buf), NULL, NULL);
	if (n != 1)
		return;
	switch (buf[0]) {
	case 'h':			/* turn on height motion lock */
		headlocked = 1;
		return;
	case 'H':			/* turn off height motion lock */
		headlocked = 0;
		return;
	case 'l':			/* retrieve last view */
		inpresflags |= DFL(DC_LASTVIEW);
		return;
	case 'p':			/* pause computation */
		inpresflags |= DFL(DC_PAUSE);
		return;
	case 'v':			/* spit out view */
		inpresflags |= DFL(DC_GETVIEW);
		return;
	case 'f':			/* frame view position */
		if (!XQueryPointer(ourdisplay, gwind, &rootw, &childw,
				&rootx, &rooty, &wx, &wy, &statemask))
			return;		/* on another screen */
		sprintf(odev_args, "%.4f %.4f", (wx+.5)/odev.hres,
				1.-(wy+.5)/odev.vres);
		inpresflags |= DFL(DC_FOCUS);
		return;
	case 'F':			/* unfocus */
		odev_args[0] = '\0';
		inpresflags |= DFL(DC_FOCUS);
		return;
	case '\n':
	case '\r':			/* resume computation */
		inpresflags |= DFL(DC_RESUME);
		return;
	case CTRL('R'):			/* redraw screen */
		odRemap(0);
		glClear(GL_DEPTH_BUFFER_BIT);
#ifdef STEREO
		setstereobuf(STEREO_BUFFER_RIGHT);
		glClear(GL_DEPTH_BUFFER_BIT);
		setstereobuf(STEREO_BUFFER_LEFT);
#endif
		return;
	case CTRL('L'):			/* refresh from server */
		if (inpresflags & DFL(DC_REDRAW))
			return;
		XRaiseWindow(ourdisplay, gwind);
		XFlush(ourdisplay);
		sleep(1);
		wipeclean();			/* fresh display */
		odRemap(1);			/* fresh tone mapping */
		dev_flush();			/* draw octrees */
		inpresflags |= DFL(DC_REDRAW);	/* resend values from server */
		rayqleft = 0;			/* hold off update */
		return;
	case 'K':			/* kill rtrace process(es) */
		inpresflags |= DFL(DC_KILL);
		break;
	case 'R':			/* restart rtrace */
		inpresflags |= DFL(DC_RESTART);
		break;
	case 'C':			/* clobber holodeck */
		inpresflags |= DFL(DC_CLOBBER);
		break;
	case 'q':			/* quit the program */
		inpresflags |= DFL(DC_QUIT);
		return;
	default:
		XBell(ourdisplay, 0);
		return;
	}
}


static
fixwindow(eexp)				/* repair damage to window */
register XExposeEvent  *eexp;
{
	int	xmin, ymin, xmax, ymax;

	if (odev.hres == 0 | odev.vres == 0) {	/* first exposure */
		resizewindow((XConfigureEvent *)eexp);
		return;
	}
	xmin = eexp->x; xmax = eexp->x + eexp->width;
	ymin = odev.vres - eexp->y - eexp->height; ymax = odev.vres - eexp->y;
						/* clear portion of depth */
	glPushAttrib(GL_DEPTH_BUFFER_BIT|GL_COLOR_BUFFER_BIT);
	glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
	glDepthFunc(GL_ALWAYS);
	glBegin(GL_POLYGON);
	glVertex3i(xmin, ymin, OMAXDEPTH);
	glVertex3i(xmax, ymin, OMAXDEPTH);
	glVertex3i(xmax, ymax, OMAXDEPTH);
	glVertex3i(xmin, ymax, OMAXDEPTH);
	glEnd();
#ifdef STEREO
	setstereobuf(STEREO_BUFFER_RIGHT);
	glBegin(GL_POLYGON);
	glVertex3i(xmin, ymin, OMAXDEPTH);
	glVertex3i(xmax, ymin, OMAXDEPTH);
	glVertex3i(xmax, ymax, OMAXDEPTH);
	glVertex3i(xmin, ymax, OMAXDEPTH);
	glEnd();
	odRedraw(1, xmin, ymin, xmax, ymax);
	setstereobuf(STEREO_BUFFER_LEFT);
#endif
	glPopAttrib();
	odRedraw(0, xmin, ymin, xmax, ymax);
}


static
resizewindow(ersz)			/* resize window */
register XConfigureEvent  *ersz;
{
	glViewport(0, 0, ersz->width, ersz->height);

	if (ersz->width == odev.hres && ersz->height == odev.vres)
		return;

	odev.hres = ersz->width;
	odev.vres = ersz->height;

	odev.v.horiz = 2.*180./PI * atan(0.5/VIEWDIST*pwidth*odev.hres);
	odev.v.vert = 2.*180./PI * atan(0.5/VIEWDIST*pheight*odev.vres);

	inpresflags |= DFL(DC_SETVIEW);
}
