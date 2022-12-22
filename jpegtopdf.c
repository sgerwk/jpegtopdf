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
	int owidth = 0, oheight = 0;
	int margin = 0;
	int twoside = 0;
	char *rotatestring = "0";
	int rotatelen = 1;

	int n, i, j;
	int in;
	struct stat ss;
	unsigned char *data;
	unsigned long len, r;
	int res;
	int width, height;
	int iwidth, iheight;
	int rotate;
	int x, y;
	double pagewidth = 595, pageheight = 842;
	double givenscale = 0;
	double scale;
	cairo_surface_t *insurface, *outsurface;
	cairo_t *cr;

				/* arguments */

	while ((opt = getopt(argc, argv, "m:x:y:w:e:s:r:o:th")) != -1)
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
		case 'w':
			owidth = atoi(optarg);
			if (owidth == 0)
				usage = 2;
			break;
		case 'e':
			oheight = atoi(optarg);
			if (oheight == 0)
				usage = 2;
			break;
		case 's':
			givenscale = atof(optarg);
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
		printf("\t\t-w width\twidth of jpeg images\n");
		printf("\t\t-e height\theight of jpeg images\n");
		printf("\t\t-s scale\toutput is as input, scaled by this\n");
		printf("\t\t-r rotations\trotate images\n");
		printf("\t\t-t\t\treconstruct a two-side document\n");
		printf("\t\t-o file.pdf\tname ouf output file\n");
		printf("\t\t-h\t\tthis help\n");
		exit(usage > 1 ? EXIT_FAILURE : EXIT_SUCCESS);
	}

				/* output file */

	fprintf(stderr, "outfile: %s\n", outfile);
	if (! ! strcmp(outfile, "-"))
		outsurface = cairo_pdf_surface_create(outfile,
				pagewidth, pageheight);
	else
		outsurface = cairo_pdf_surface_create_for_stream(stdoutwrite,
				NULL, pagewidth, pageheight);

				/* loop over input images */

	n = argc;
	for (i = 0; i < n; i++) {
		j = ! twoside ? i : i % 2 == 0 ? i / 2 : n - i / 2 - 1;
		infile = argv[j];
		fprintf(stderr, "%s\n", infile);

					/* read input file */

		stat(infile, &ss);
		len = ss.st_size;
		data = malloc(len);
		if (data == NULL) {
			fprintf(stderr,
				"not enough memory for image %s\n", infile);
			continue;
		}
		in = open(infile, O_RDONLY);
		if (in == -1) {
			perror(infile);
			continue;
		}
		r = read(in, data, len);
		if (r != len) {
			fprintf(stderr,
				"short read on %s: %lu bytes instead of %lu\n",
				infile, r, len);
			continue;
		}
		close(in);

					/* width and height */

		if (owidth == 0 || oheight == 0) {
			res = jpegsize(data, len, &width, &height);
			if (res) {
				fprintf(stderr, "error parsing jpeg file\n");
				continue;
			}
		}
		if (owidth != 0)
			width = owidth;
		if (oheight != 0)
			height = oheight;
		iwidth = width;
		iheight = height;
		fprintf(stderr, "%dx%d\n", width, height);

					/* rotation */

		rotate = 0;
		j = i < rotatelen ? i : rotatelen - 1;
		if (rotatestring[j] >= '0' && rotatestring[j] <= '3')
			rotate = rotatestring[j] - '0';
		if (rotatestring[j] == 'a' && width > height)
			rotate = 1;
		if (rotatestring[j] == 'A' && width > height)
			rotate = 3;
		if (rotate % 2 != 0) {
			width = iheight;
			height = iwidth;
		}

					/* page size */

		if (givenscale) {
			pagewidth =  width  * givenscale + 2 * margin;
			pageheight = height * givenscale + 2 * margin;
			cairo_pdf_surface_set_size(outsurface,
				pagewidth, pageheight);
		}

					/* scale */

		scale = MAX(width  / (pagewidth  - 2 * margin),
		            height / (pageheight - 2 * margin));
		x = ox + (pagewidth  - width  / scale) / 2;
		y = oy + (pageheight - height / scale) / 2;
		fprintf(stderr, "%d,%d -> %dx%d /", x, y, width, height);
		fprintf(stderr, "%g\n", scale);

					/* input surface */

		insurface = cairo_image_surface_create(CAIRO_FORMAT_RGB24,
			iwidth, iheight);
		if (cairo_surface_status(outsurface) != CAIRO_STATUS_SUCCESS) {
			fprintf(stderr, "error %s\n", argv[0]);
			exit(EXIT_FAILURE);
		}
		cairo_surface_set_mime_data(insurface, CAIRO_MIME_TYPE_JPEG,
			data, len, free, data);

					/* output page */

		cr = cairo_create(outsurface);
		cairo_translate(cr, x, y);
		cairo_scale(cr, 1L / scale, 1L / scale);
		cairo_translate(cr, width / 2, height / 2);
		cairo_rotate(cr, rotate * M_PI / 2);
		cairo_translate(cr, -iwidth / 2, -iheight / 2);
		cairo_set_source_surface(cr, insurface, 0, 0);
		cairo_rectangle(cr, 0, 0, iwidth, iheight);
		cairo_fill(cr);
		cairo_destroy(cr);

		cairo_surface_show_page(outsurface);
		cairo_surface_destroy(insurface);
	}

	cairo_surface_destroy(outsurface);
	return EXIT_SUCCESS;
}

