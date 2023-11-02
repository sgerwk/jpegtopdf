/*
 * jpegtopdf.c
 *
 * make a pdf file out of a sequence of jpeg images
 * see jpegtopdf.1
 */

/*
 * internals
 * ---------
 *
 * each jpeg file is read into a memory buffer and attached to a cairo image
 * surface via cairo_surface_set_mime_data(); for output devices that support
 * such attachments (such as PDF and SVG), the memory buffer is directly used
 * in place of the content of the surface
 *
 * this removes the need of decoding the jpeg file, except that its width and
 * height are required to create the surface and find the scale and placement
 * of the image in the page; these are found by libjpeg functions in
 * jpegsize(), which only reads the jpeg header without decoding all the file
 */

#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <math.h>
#include <string.h>

#include <jpeglib.h>
#include <cairo.h>
#include <cairo-pdf.h>

#define MAX(a,b) (((a) > (b)) ? (a) : (b))

/*
 * rectangle structure
 */
struct rectangle {
	double x0;
	double y0;
	double x1;
	double y1;
};

/*
 * paper size
 */
struct
{	char *name;		struct rectangle size;		}
papersize[] = {
{	"Letter",		{0.0, 0.0,  612.0,  792.0}	},
{	"LetterSmall",		{0.0, 0.0,  612.0,  792.0}	},
{	"Tabloid",		{0.0, 0.0,  792.0, 1224.0}	},
{	"Ledger",		{0.0, 0.0, 1224.0,  792.0}	},
{	"Legal",		{0.0, 0.0,  612.0, 1008.0}	},
{	"Statement",		{0.0, 0.0,  396.0,  612.0}	},
{	"Executive",		{0.0, 0.0,  540.0,  720.0}	},
{	"Folio",		{0.0, 0.0,  612.0,  936.0}	},
{	"Quarto",		{0.0, 0.0,  610.0,  780.0}	},
{	"10x14",		{0.0, 0.0,  720.0, 1008.0}	},

{	"A0",			{0.0, 0.0, 2384.0, 3371.0}	},
{	"A1",			{0.0, 0.0, 1685.0, 2384.0}	},
{	"A2",			{0.0, 0.0, 1190.0, 1684.0}	},
{	"A3",			{0.0, 0.0,  842.0, 1190.0}	},
{	"A4",			{0.0, 0.0,  595.0,  842.0}	},
{	"A5",			{0.0, 0.0,  420.0,  595.0}	},
{	"A6",			{0.0, 0.0,  298.0,  420.0}	},
{	"A7",			{0.0, 0.0,  210.0,  298.0}	},
{	"A8",			{0.0, 0.0,  148.0,  210.0}	},
{	"A9",			{0.0, 0.0,  105.0,  147.0}	},
{	"A10",			{0.0, 0.0,   74.0,  105.0}	},

{	"B0",			{0.0, 0.0, 2835.0, 4008.0}	},
{	"B1",			{0.0, 0.0, 2004.0, 2835.0}	},
{	"B2",			{0.0, 0.0, 1417.0, 2004.0}	},
{	"B3",			{0.0, 0.0, 1001.0, 1417.0}	},
{	"B4",			{0.0, 0.0,  729.0, 1032.0}	},
{	"B5",			{0.0, 0.0,  516.0,  729.0}	},
{	"B6",			{0.0, 0.0,  354.0,  499.0}	},
{	"B7",			{0.0, 0.0,  249.0,  354.0}	},
{	"B8",			{0.0, 0.0,  176.0,  249.0}	},
{	"B9",			{0.0, 0.0,  125.0,  176.0}	},
{	"B10",			{0.0, 0.0,   88.0,  125.0}	},

{	"C0",			{0.0, 0.0, 2599.0, 3677.0}	},
{	"C1",			{0.0, 0.0, 1837.0, 2599.0}	},
{	"C2",			{0.0, 0.0, 1837.0,  578.0}	},
{	"C3",			{0.0, 0.0,  578.0,  919.0}	},
{	"C4",			{0.0, 0.0,  919.0,  649.0}	},
{	"C5",			{0.0, 0.0,  649.0,  459.0}	},
{	"C6",			{0.0, 0.0,  459.0,  323.0}	},
{	"C7",			{0.0, 0.0,  230.0,  323.0}	},
{	"C8",			{0.0, 0.0,  162.0,  230.0}	},
{	"C9",			{0.0, 0.0,  113.0,  162.0}	},
{	"C10",			{0.0, 0.0,   79.0,  113.0}	},

{	NULL,			{0.0, 0.0,    0.0,    0.0}	}
};

/*
 * from name to paper size
 */
struct rectangle *get_papersize(char *name) {
	int i;

	for (i = 0; papersize[i].name; i++)
		if (! strcasecmp(papersize[i].name, name))
			return &papersize[i].size;

	return NULL;
}

/*
 * default paper size
 */
char *defaultpapersize() {
	FILE *fd;
	char s[100], *r;
	char *res;

	fd = fopen("/etc/papersize", "r");
	if (fd == NULL)
		return NULL;

	r = malloc(100);
	do {
		res = fgets(s, 90, fd);
		if (res == NULL) {
			fclose(fd);
			free(r);
			return NULL;
		}
		res = strchr(s, '#');
		if (res)
			*res = '\0';
	} while (sscanf(s, "%90s", r) != 1);

	fclose(fd);
	return r;
}

/*
 * dimensions of a jpeg image
 */
int jpegsize(unsigned char *data, int len, int *width, int *height) {
	struct jpeg_decompress_struct dec;
	struct jpeg_error_mgr jem;

	dec.err = jpeg_std_error(&jem);
	jpeg_create_decompress(&dec);
	jpeg_mem_src(&dec, data, len);
	jpeg_read_header(&dec, TRUE);

	*width = dec.image_width;
	*height = dec.image_height;
	return 0;
}

/*
 * write to stdout
 */
cairo_status_t stdoutwrite(void *closure,
		const unsigned char *data, unsigned len) {
	int res;
	(void) closure;
	res = write(STDOUT_FILENO, data, len);
	if (res >= 0 || (unsigned) res == len)
		return CAIRO_STATUS_SUCCESS;
	return CAIRO_STATUS_WRITE_ERROR;
}

/*
 * main
 */
int main(int argc, char *argv[]) {
	int opt;
	int usage = 0;
	char *outfile = "output.pdf", *infile;
	int ox = 0, oy = 0;
	int argwidth = 0, argheight = 0;	// size of image from cmdline
	int margin = 0;
	int twoside = 0;
	char *rotatestring = "0";
	int rotatelen = 1;

	int n, i, j;
	int in;
	unsigned char *data;
	unsigned long len, chunklen = 1024 * 16, r;
	int inminus;
	int res;
	int width, height;			// size of image
	int rwidth, rheight;			// size of rotated image
	int rotate;
	int x, y;
	char *papersize = NULL;
	struct rectangle *paperrectangle;
	double pagewidth, pageheight;
	double pagescale = 0, argscale = 0, scale, tscale;
	cairo_surface_t *insurface, *outsurface;
	cairo_t *cr;

				/* arguments */

	while ((opt = getopt(argc, argv, "m:x:y:w:e:p:s:r:o:th")) != -1)
		switch(opt) {
		case 'm':
			margin = atoi(optarg);
			break;
		case 'x':
			ox = atoi(optarg);
			if (ox == 0)
				usage = 2;
			break;
		case 'y':
			oy = atoi(optarg);
			if (oy == 0)
				usage = 2;
			break;
		case 'l':
			argwidth = atoi(optarg);
			if (argwidth == 0)
				usage = 2;
			break;
		case 'a':
			argheight = atoi(optarg);
			if (argheight == 0)
				usage = 2;
			break;
		case 's':
			argscale = atof(optarg);
			break;
		case 'p':
			papersize = optarg;
			break;
		case 'w':
			pagewidth = atoi(optarg);
			if (pagewidth == 0)
				usage = 2;
			break;
		case 'e':
			pageheight = atoi(optarg);
			if (pageheight == 0)
				usage = 2;
			break;
		case 'r':
			rotatestring = optarg;
			rotatelen = strlen(rotatestring);
			if (rotatelen < 1) {
				fprintf(stderr,
					"error: empty rotation string\n");
				usage = 2;
			}
			break;
		case 'o':
			outfile = optarg;
			break;
		case 't':
			twoside = 1;
			break;
		case 'h':
			usage = 1;
			break;
		}
	argc -= optind;
	argv += optind;
	if (! usage && argc - 1 < 0) {
		fprintf(stderr, "error - no input file\n");
		usage = 2;
	}
	if (usage > 0) {
		printf("usage:\n");
		printf("\tjpegtopdf [options] file.jpg ...\n");
		printf("\t\t-m margin\tspace around images\n");
		printf("\t\t-x x\t\tspace from left edge\n");
		printf("\t\t-y y\t\tspace from top\n");
		printf("\t\t-l width\twidth of jpeg images\n");
		printf("\t\t-a height\theight of jpeg images\n");
		printf("\t\t-s scale\tscale input image\n");
		printf("\t\t-p paper\tpage size, like A4 or letter");
		printf(" or widthxheight or scale\n");
		printf("\t\t-w width\twidth of page\n");
		printf("\t\t-e height\theight of page\n");
		printf("\t\t-r rotations\trotate images\n");
		printf("\t\t-t\t\treconstruct a two-side document\n");
		printf("\t\t-o file.pdf\tname of output file\n");
		printf("\t\t-h\t\tthis help\n");
		exit(usage > 1 ? EXIT_FAILURE : EXIT_SUCCESS);
	}

				/* page size */

	if (papersize == NULL)
		papersize = defaultpapersize();
	paperrectangle = get_papersize(papersize ? papersize : "A4");
	if (paperrectangle != NULL) {
		pagewidth = paperrectangle->x1;
		pageheight = paperrectangle->y1;
	}
	else if (2 == sscanf(papersize, "%lgx%lg", &pagewidth, &pageheight)) {
	}
	else if (1 == sscanf(papersize, "%lg", &pagescale)) {
		pagewidth = 0;
		pageheight = 0;
	}

	if (pagescale == 0)
		fprintf(stderr, "page size: %g x %g\n", pagewidth, pageheight);
	else
		fprintf(stderr, "page size: image x %g\n", pagescale);

				/* output file */

	fprintf(stderr, "outfile: %s\n", outfile);
	if (! ! strcmp(outfile, "-"))
		outsurface = cairo_pdf_surface_create(outfile,
				pagewidth, pageheight);
	else
		outsurface = cairo_pdf_surface_create_for_stream(stdoutwrite,
				NULL, pagewidth, pageheight);

				/* loop over input images */

	inminus = 0;
	n = argc;
	for (i = 0; i < n; i++) {
		j = ! twoside ? i : i % 2 == 0 ? i / 2 : n - i / 2 - 1;
		infile = argv[j];
		fprintf(stderr, "%s\n", infile);

					/* read input file */

		if (! strcmp(infile, "-")) {
			if (inminus > 0) {
				printf("error: stdin given more than once\n");
				continue;
			}
			in = STDIN_FILENO;
			inminus++;
		}
		else {
			in = open(infile, O_RDONLY);
			if (in == -1) {
				perror(infile);
				continue;
			}
		}
		len = 0;
		data = NULL;
		do {
			data = realloc(data, len + chunklen);
			r = read(in, data + len, chunklen);
			len += r;
		} while (r == chunklen);
		close(in);

					/* width and height */

		if (argwidth != 0 && argheight != 0) {
			width = argwidth;
			height = argheight;
		}
		else {
			res = jpegsize(data, len, &width, &height);
			if (res) {
				fprintf(stderr, "error parsing jpeg file\n");
				continue;
			}
		}
		fprintf(stderr, "image size: %dx%d\n", width, height);

					/* rotation */

		rotate = 0;
		j = i < rotatelen ? i : rotatelen - 1;
		if (rotatestring[j] >= '0' && rotatestring[j] <= '3')
			rotate = rotatestring[j] - '0';
		if (rotatestring[j] == 'a' && width > height)
			rotate = 1;
		if (rotatestring[j] == 'A' && width > height)
			rotate = 3;
		rwidth =  rotate % 2 == 0 ? width : height;
		rheight = rotate % 2 == 0 ? height : width;
		fprintf(stderr, "size on page: %dx%d\n", rwidth, rheight);

					/* page size */

		if (pagescale != 0) {
			tscale = argscale != 0 ? argscale : 1;
			pagewidth =  rwidth  * tscale * pagescale + 2 * margin;
			pageheight = rheight * tscale * pagescale + 2 * margin;
		}
		fprintf(stderr, "page size: %gx%g\n", pagewidth, pageheight);
		cairo_pdf_surface_set_size(outsurface, pagewidth, pageheight);

					/* scale */

		scale = argscale != 0 ? argscale :
			pagescale != 0 ? (argscale != 0 ? argscale : 1) :
			1.0 / MAX(rwidth  / (pagewidth  - 2 * margin),
			          rheight / (pageheight - 2 * margin));
		x = ox + (pagewidth  - rwidth  * scale) / 2;
		y = oy + (pageheight - rheight * scale) / 2;
		fprintf(stderr, "%d,%d -> %dx%d *", x, y, rwidth, rheight);
		fprintf(stderr, " %g\n", scale);

					/* input surface */

		insurface = cairo_image_surface_create(CAIRO_FORMAT_RGB24,
			width, height);
		if (cairo_surface_status(outsurface) != CAIRO_STATUS_SUCCESS) {
			fprintf(stderr, "error %s\n", argv[0]);
			exit(EXIT_FAILURE);
		}
		cairo_surface_set_mime_data(insurface, CAIRO_MIME_TYPE_JPEG,
			data, len, free, data);

					/* output page */

		cr = cairo_create(outsurface);
		cairo_translate(cr, x, y);
		cairo_scale(cr, scale, scale);
		cairo_translate(cr, rwidth / 2, rheight / 2);
		cairo_rotate(cr, rotate * M_PI / 2);
		cairo_translate(cr, -width / 2, -height / 2);
		cairo_set_source_surface(cr, insurface, 0, 0);
		cairo_rectangle(cr, 0, 0, width, height);
		cairo_fill(cr);
		cairo_destroy(cr);

		cairo_surface_show_page(outsurface);
		cairo_surface_destroy(insurface);
	}

	cairo_surface_destroy(outsurface);
	return EXIT_SUCCESS;
}

