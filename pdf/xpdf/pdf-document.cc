/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8; c-indent-level: 8 -*- */
/* pdfdocument.h: Implementation of EvDocument for PDF
 * Copyright (C) 2004, Red Hat, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

#include <glib/gi18n.h>

#include "gpdf-g-switch.h"
#include "pdf-document.h"
#include "ev-ps-exporter.h"
#include "ev-document-find.h"
#include "gpdf-g-switch.h"
#include "ev-document-bookmarks.h"
#include "ev-document-misc.h"
#include "ev-document-thumbnails.h"

#include "GlobalParams.h"
#include "GDKSplashOutputDev.h"
#include "PDFDoc.h"
#include "Outline.h"
#include "UnicodeMap.h"
#include "GlobalParams.h"
#include "GfxState.h"
#include "Thumb.h"
#include "goo/GList.h"
#include "PSOutputDev.h"

enum {
	PROP_0,
	PROP_TITLE
};

typedef struct
{
	PdfDocument *document;
	gunichar *ucs4;
	glong ucs4_len;
	guint idle;
        /* full results are only possible for the rendered current page */
        int current_page;
        GArray *current_page_results;
        guchar *other_page_flags; /* length n_pages + 1, first element ignored */
        int start_page;   /* skip this one as we iterate, since we did it first */
        int search_page;  /* the page we're searching now */
        TextOutputDev *output_dev;
} PdfDocumentSearch;

typedef struct _PdfDocumentClass PdfDocumentClass;

#define PDF_DOCUMENT_CLASS(klass)     (G_TYPE_CHECK_CLASS_CAST ((klass), PDF_TYPE_DOCUMENT, PdfDocumentClass))
#define PDF_IS_DOCUMENT_CLASS(klass)  (G_TYPE_CHECK_CLASS_TYPE ((klass), PDF_TYPE_DOCUMENT))
#define PDF_DOCUMENT_GET_CLASS(obj)   (G_TYPE_INSTANCE_GET_CLASS ((obj), PDF_TYPE_DOCUMENT, PdfDocumentClass))

struct _PdfDocumentClass
{
	GObjectClass parent_class;
};

struct _PdfDocument
{
	GObject parent_instance;

	int page;
	int page_x_offset;
	int page_y_offset;
	double scale;
	GdkDrawable *target;

	GDKSplashOutputDev *out;
	PSOutputDev *ps_out;
	PDFDoc *doc;
	UnicodeMap *umap;

	gboolean page_valid;

	PdfDocumentSearch *search;
};

static void pdf_document_document_bookmarks_iface_init  (EvDocumentBookmarksIface  *iface);
static void pdf_document_document_thumbnails_iface_init (EvDocumentThumbnailsIface *iface);
static void pdf_document_document_iface_init            (EvDocumentIface           *iface);
static void pdf_document_ps_exporter_iface_init (EvPSExporterIface   *iface);
static void pdf_document_find_iface_init        (EvDocumentFindIface *iface);
static void pdf_document_search_free            (PdfDocumentSearch   *search);
static void pdf_document_search_page_changed    (PdfDocumentSearch   *search);

G_DEFINE_TYPE_WITH_CODE (PdfDocument, pdf_document, G_TYPE_OBJECT,
                         {
				 G_IMPLEMENT_INTERFACE (EV_TYPE_DOCUMENT,
							pdf_document_document_iface_init);
				 G_IMPLEMENT_INTERFACE (EV_TYPE_DOCUMENT_BOOKMARKS,
							pdf_document_document_bookmarks_iface_init);
				 G_IMPLEMENT_INTERFACE (EV_TYPE_DOCUMENT_THUMBNAILS,
							pdf_document_document_thumbnails_iface_init);
				 G_IMPLEMENT_INTERFACE (EV_TYPE_PS_EXPORTER,
							pdf_document_ps_exporter_iface_init);
				 G_IMPLEMENT_INTERFACE (EV_TYPE_DOCUMENT_FIND,
							pdf_document_find_iface_init);
			 });

static gboolean
document_validate_page (PdfDocument *pdf_document)
{
	if (!pdf_document->page_valid) {
		pdf_document->doc->displayPage (pdf_document->out, pdf_document->page,
						72 * pdf_document->scale,
						72 * pdf_document->scale,
						0, gTrue, gTrue);

		pdf_document->page_valid = TRUE;

                /* Update the search results available to the app since
                 * we only provide full results on the current page
                 */
                if (pdf_document->search)
                        pdf_document_search_page_changed (pdf_document->search);
	}

	return pdf_document->page_valid;
}

static gboolean
pdf_document_load (EvDocument  *document,
		   const char  *uri,
		   GError     **error)
{
	PdfDocument *pdf_document = PDF_DOCUMENT (document);
	PDFDoc *newDoc;
	int err;
	char *filename;
	GString *filename_g;
	GString *enc;

	if (!globalParams) {
		globalParams = new GlobalParams("/etc/xpdfrc");
		globalParams->setupBaseFontsFc(NULL);
	}

	if (! pdf_document->umap) {
		enc = new GString("UTF-8");
		pdf_document->umap = globalParams->getUnicodeMap(enc);
		pdf_document->umap->incRefCnt ();
		delete enc;
	}

	filename = g_filename_from_uri (uri, NULL, error);
	if (!filename)
		return FALSE;

	filename_g = new GString (filename);
	g_free (filename);

	// open the PDF file, assumes ownership of filename_g
	newDoc = new PDFDoc(filename_g, 0, 0);

	if (!newDoc->isOk()) {
		err = newDoc->getErrorCode();
		delete newDoc;

		/* FIXME: Add a real error enum to EvDocument */
		g_set_error (error, G_FILE_ERROR,
			     G_FILE_ERROR_FAILED,
			     "Failed to load document (error %d) '%s'\n",
			     err,
			     uri);

		return FALSE;
	}

	if (pdf_document->doc)
		delete pdf_document->doc;
	pdf_document->doc = newDoc;

	pdf_document->page = 1;

	if (pdf_document->out)
		pdf_document->out->startDoc(pdf_document->doc->getXRef());

	pdf_document->page_valid = FALSE;

	g_object_notify (G_OBJECT (pdf_document), "title");

	return TRUE;
}

static gboolean
pdf_document_save (EvDocument  *document,
		   const char  *uri,
		   GError     **error)
{
	PdfDocument *pdf_document = PDF_DOCUMENT (document);
	char *filename;
	gboolean retval = FALSE;

	filename = g_filename_from_uri (uri, NULL, error);
	if (filename != NULL) {
		GString *fname = new GString (filename);

		retval = pdf_document->doc->saveAs (fname);
	}

	return retval;
}

static int
pdf_document_get_n_pages (EvDocument  *document)
{
	PdfDocument *pdf_document = PDF_DOCUMENT (document);

	if (pdf_document->doc)
		return pdf_document->doc->getNumPages();
	else
		return 1;
}

static void
pdf_document_set_page (EvDocument  *document,
		       int          page)
{
	PdfDocument *pdf_document = PDF_DOCUMENT (document);

	page = CLAMP (page, 1, ev_document_get_n_pages (document));

	if (page != pdf_document->page) {
		pdf_document->page = page;
		pdf_document->page_valid = FALSE;
	}

	ev_document_changed (document);
}

static int
pdf_document_get_page (EvDocument  *document)
{
	PdfDocument *pdf_document = PDF_DOCUMENT (document);

	return pdf_document->page;
}

static void
redraw_callback (void *data)
{
	/* Need to hook up through a EvDocument callback? */
}

static void
pdf_document_set_target (EvDocument  *document,
			 GdkDrawable *target)
{
	PdfDocument *pdf_document = PDF_DOCUMENT (document);

	if (pdf_document->target != target) {
		if (pdf_document->target)
			g_object_unref (pdf_document->target);

		pdf_document->target = target;

		if (pdf_document->target)
			g_object_ref (pdf_document->target);

		if (pdf_document->out) {
			delete pdf_document->out;
			pdf_document->out = NULL;
		}

		if (pdf_document->target) {
			pdf_document->out = new GDKSplashOutputDev (gdk_drawable_get_screen (pdf_document->target),
							 redraw_callback, (void*) document);

			if (pdf_document->doc)
				pdf_document->out->startDoc(pdf_document->doc->getXRef());

		}

		pdf_document->page_valid = FALSE;
	}
}

static void
pdf_document_set_scale (EvDocument  *document,
			double       scale)
{
	PdfDocument *pdf_document = PDF_DOCUMENT (document);

	if (pdf_document->scale != scale) {
		pdf_document->scale = scale;
		pdf_document->page_valid = FALSE;
	}
}

static void
pdf_document_set_page_offset (EvDocument  *document,
			      int          x,
			      int          y)
{
	PdfDocument *pdf_document = PDF_DOCUMENT (document);

	pdf_document->page_x_offset = x;
	pdf_document->page_y_offset = y;
}

static void
pdf_document_get_page_size (EvDocument   *document,
			    int          *width,
			    int          *height)
{
	PdfDocument *pdf_document = PDF_DOCUMENT (document);

	if (document_validate_page (pdf_document)) {
		if (width)
			*width = pdf_document->out->getBitmapWidth();
		if (height)
			*height = pdf_document->out->getBitmapHeight();
	} else {
		if (width)
			*width = 1;
		if (height)
			*height = 1;
	}
}

static void
pdf_document_render (EvDocument  *document,
		     int          clip_x,
		     int          clip_y,
		     int          clip_width,
		     int          clip_height)
{
	PdfDocument *pdf_document = PDF_DOCUMENT (document);
	GdkRectangle page;
	GdkRectangle draw;

	if (!document_validate_page (pdf_document) || !pdf_document->target)
		return;

	page.x = pdf_document->page_x_offset;
	page.y = pdf_document->page_y_offset;
	page.width = pdf_document->out->getBitmapWidth();
	page.height = pdf_document->out->getBitmapHeight();

	draw.x = clip_x;
	draw.y = clip_y;
	draw.width = clip_width;
	draw.height = clip_height;

	if (gdk_rectangle_intersect (&page, &draw, &draw))
		pdf_document->out->redraw (draw.x - page.x, draw.y - page.y,
					   pdf_document->target,
					   draw.x, draw.y,
					   draw.width, draw.height);
}

static void
pdf_document_search_emit_found (PdfDocumentSearch *search)
{
        PdfDocument *pdf_document = search->document;
        int n_pages;
        int pages_done;
        GArray *tmp_results;
        int i;

        n_pages = ev_document_get_n_pages (EV_DOCUMENT (search->document));
        if (search->search_page > search->start_page) {
                pages_done = search->search_page - search->start_page;
        } else if (search->search_page == search->start_page) {
                pages_done = n_pages;
        } else {
                pages_done = n_pages - search->start_page + search->search_page;
        }

        tmp_results = g_array_new (FALSE, FALSE, sizeof (EvFindResult));
        g_array_append_vals (tmp_results,
                             search->current_page_results->data,
                             search->current_page_results->len);

        /* Now append a bogus element for each page that has a result in it,
         * that is not the current page
         */
        i = 1;
        while (i <= n_pages) {
                if (i != pdf_document->page &&
                    search->other_page_flags[i]) {
                        EvFindResult result;

                        result.page_num = i;

                        /* Use bogus coordinates, again we can't get coordinates
                         * until this is the current page because TextOutputDev
                         * isn't good enough
                         */
                        result.highlight_area.x = -1;
                        result.highlight_area.y = -1;
                        result.highlight_area.width = 1;
                        result.highlight_area.height = 1;

                        g_array_append_val (tmp_results, result);
                }

                ++i;
        }

        ev_document_find_found (EV_DOCUMENT_FIND (pdf_document),
                                (EvFindResult*) tmp_results->data,
                                tmp_results->len,
                                pages_done / (double) n_pages);

        g_array_free (tmp_results, TRUE);
}

static void
pdf_document_search_page_changed (PdfDocumentSearch   *search)
{
        PdfDocument *pdf_document = search->document;
        int current_page;
        EvFindResult result;
        int xMin, yMin, xMax, yMax;

        current_page = pdf_document->page;

        if (!pdf_document->page_valid) {
                /* we can't do anything until displayPage() */
                search->current_page = -1;
                return;
        }

        if (search->current_page == current_page)
                return;

        /* We need to create current_page_results for the new current page */
        g_array_set_size (search->current_page_results, 0);

        if (pdf_document->out->findText (search->ucs4, search->ucs4_len,
                                         gTrue, gTrue, // startAtTop, stopAtBottom
                                         gFalse, gFalse, // startAtLast, stopAtLast
                                         &xMin, &yMin, &xMax, &yMax)) {
                result.page_num = pdf_document->page;

                result.highlight_area.x = xMin;
                result.highlight_area.y = yMin;
                result.highlight_area.width = xMax - xMin;
                result.highlight_area.height = yMax - yMin;

                g_array_append_val (search->current_page_results, result);
                /* Now find further results */

                while (pdf_document->out->findText (search->ucs4, search->ucs4_len,
                                                    gFalse, gTrue,
                                                    gTrue, gFalse,
                                                    &xMin, &yMin, &xMax, &yMax)) {

                        result.page_num = pdf_document->page;

                        result.highlight_area.x = xMin;
                        result.highlight_area.y = yMin;
                        result.highlight_area.width = xMax - xMin;
                        result.highlight_area.height = yMax - yMin;

                        g_array_append_val (search->current_page_results, result);
                }
        }

        /* needed for the initial current page since we don't search
         * it in the idle
         */
        search->other_page_flags[current_page] =
                search->current_page_results->len > 0;

        pdf_document_search_emit_found (search);
}

static gboolean
pdf_document_search_idle_callback (void *data)
{
        PdfDocumentSearch *search = (PdfDocumentSearch*) data;
        PdfDocument *pdf_document = search->document;
        int n_pages;
        double xMin, yMin, xMax, yMax;

        /* Note that PDF page count is 1 through n_pages INCLUSIVE
         * like a real book. We are looking to add one result for each
         * page with a match, because the coordinates are meaningless
         * with TextOutputDev, so we just want to flag matching pages
         * and then when the user switches to the current page, we
         * will emit "found" again with the real results.
         */
        n_pages = ev_document_get_n_pages (EV_DOCUMENT (search->document));

        if (search->search_page == search->start_page) {
                goto end_search;
        }

        if (search->output_dev == 0) {
                /* First time through here... */
                search->output_dev = new TextOutputDev (NULL, gTrue, gFalse, gFalse);
                if (!search->output_dev->isOk()) {
                        goto end_search;
                }
        }

        pdf_document->doc->displayPage (search->output_dev,
                                        search->search_page,
                                        72, 72, 0, gTrue, gFalse);

        if (search->output_dev->findText (search->ucs4,
                                          search->ucs4_len,
                                          gTrue, gTrue, // startAtTop, stopAtBottom
                                          gFalse, gFalse, // startAtLast, stopAtLast
                                          &xMin, &yMin, &xMax, &yMax)) {
                /* This page has results */
                search->other_page_flags[search->search_page] = TRUE;
        }

        search->search_page += 1;
        if (search->search_page > n_pages) {
                /* wrap around */
                search->search_page = 1;
        }

        /* We do this even if nothing was found, to update the percent complete */
        pdf_document_search_emit_found (search);

        return TRUE;

 end_search:
        /* We're done. */
        search->idle = 0; /* will return FALSE to remove */
        return FALSE;
}

static void
pdf_document_find_begin (EvDocumentFind   *document,
                         const char       *search_string,
                         gboolean          case_sensitive)
{
        PdfDocument *pdf_document = PDF_DOCUMENT (document);
        PdfDocumentSearch *search;
        int n_pages;
        gunichar *ucs4;
        glong ucs4_len;

        /* FIXME handle case_sensitive (right now XPDF
         * code is always case insensitive for ASCII
         * and case sensitive for all other languaages)
         */

        g_assert (sizeof (gunichar) == sizeof (Unicode));
        ucs4 = g_utf8_to_ucs4_fast (search_string, -1,
                                    &ucs4_len);

        if (pdf_document->search &&
            pdf_document->search->ucs4_len == ucs4_len &&
            memcmp (pdf_document->search->ucs4,
                    ucs4,
                    sizeof (gunichar) * ucs4_len) == 0) {
                /* Search is unchanged */
                g_free (ucs4);
                return;
        }

        if (pdf_document->search) {
                pdf_document_search_free (pdf_document->search);
                pdf_document->search = NULL;
        }

        search = g_new0 (PdfDocumentSearch, 1);

        search->ucs4 = ucs4;
        search->ucs4_len = ucs4_len;

        search->current_page_results = g_array_new (FALSE,
                                                    FALSE,
                                                    sizeof (EvFindResult));
        n_pages = ev_document_get_n_pages (EV_DOCUMENT (document));

        /* This is an array of bool; with the first value ignored
         * so we can index by the based-at-1 page numbers
         */
        search->other_page_flags = g_new0 (guchar, n_pages + 1);

        search->document = pdf_document;

        /* We add at low priority so the progress bar repaints */
        search->idle = g_idle_add_full (G_PRIORITY_LOW,
                                        pdf_document_search_idle_callback,
                                        search,
                                        NULL);

        search->output_dev = 0;

        search->start_page = pdf_document->page;
        search->search_page = search->start_page + 1;
        if (search->search_page > n_pages)
                search->search_page = 1;

        search->current_page = -1;

        pdf_document->search = search;

        /* Update for the current page right away */
        pdf_document_search_page_changed (search);
}

static void
pdf_document_find_cancel (EvDocumentFind   *document)
{
        PdfDocument *pdf_document = PDF_DOCUMENT (document);

	if (pdf_document->search) {
		pdf_document_search_free (pdf_document->search);
		pdf_document->search = NULL;
	}
}

static void
pdf_document_search_free (PdfDocumentSearch   *search)
{
        if (search->idle != 0)
                g_source_remove (search->idle);

	if (search->output_dev)
		delete search->output_dev;

        g_array_free (search->current_page_results, TRUE);
        g_free (search->other_page_flags);

        g_free (search->ucs4);
	g_free (search);
}

static void
pdf_document_ps_export_begin (EvPSExporter *exporter, const char *filename)
{
	PdfDocument *document = PDF_DOCUMENT (exporter);

	if (document->ps_out)
		delete document->ps_out;

	document->ps_out = new PSOutputDev ((char *)filename, document->doc->getXRef(),
					    document->doc->getCatalog(), 1,
					    ev_document_get_n_pages (EV_DOCUMENT (document)),
					    psModePS);
}

static void
pdf_document_ps_export_do_page (EvPSExporter *exporter, int page)
{
	PdfDocument *document = PDF_DOCUMENT (exporter);

	document->doc->displayPage (document->ps_out, page,
				    72.0, 72.0, 0, gTrue, gFalse);
}

static void
pdf_document_ps_export_end (EvPSExporter *exporter)
{
	PdfDocument *document = PDF_DOCUMENT (exporter);

	delete document->ps_out;
	document->ps_out = NULL;
}


/* EvDocumentBookmarks Implementation */
typedef struct
{
	/* goo GList, not glib */
	GList *items;
	int index;
	int level;
} BookmarksIter;

static gchar *
unicode_to_char (OutlineItem *outline_item,
		 UnicodeMap *uMap)
{
	GString gstr;
	gchar buf[8]; /* 8 is enough for mapping an unicode char to a string */
	int i, n;

	for (i = 0; i < outline_item->getTitleLength(); ++i) {
		n = uMap->mapUnicode(outline_item->getTitle()[i], buf, sizeof(buf));
		gstr.append(buf, n);
	}

	return g_strdup (gstr.getCString ());
}


static gboolean
pdf_document_bookmarks_has_document_bookmarks (EvDocumentBookmarks *document_bookmarks)
{
	PdfDocument *pdf_document = PDF_DOCUMENT (document_bookmarks);
	Outline *outline;

	g_return_val_if_fail (PDF_IS_DOCUMENT (document_bookmarks), FALSE);

	outline = pdf_document->doc->getOutline();
	if (outline->getItems() != NULL &&
	    outline->getItems()->getLength() > 0)
		return TRUE;

	return FALSE;
}

static EvDocumentBookmarksIter *
pdf_document_bookmarks_begin_read (EvDocumentBookmarks *document_bookmarks)
{
	PdfDocument *pdf_document = PDF_DOCUMENT (document_bookmarks);
	Outline *outline;
	BookmarksIter *iter;
	GList *items;

	g_return_val_if_fail (PDF_IS_DOCUMENT (document_bookmarks), NULL);

	outline = pdf_document->doc->getOutline();
	items = outline->getItems();
	if (! items)
		return NULL;

	iter = g_new0 (BookmarksIter, 1);
	iter->items = items;
	iter->index = 0;
	iter->level = 0;

	return (EvDocumentBookmarksIter *) iter;
}

static gboolean
pdf_document_bookmarks_get_values (EvDocumentBookmarks      *document_bookmarks,
				   EvDocumentBookmarksIter  *bookmarks_iter,
				   char                    **title,
				   EvDocumentBookmarksType  *type,
				   gint                     *page)
{
	PdfDocument *pdf_document = PDF_DOCUMENT (document_bookmarks);
	BookmarksIter *iter = (BookmarksIter *)bookmarks_iter;
	OutlineItem *anItem;
	LinkAction *link_action;
	LinkDest *link_dest = NULL;
	LinkURI *link_uri = NULL;
	LinkGoTo *link_goto = NULL;
	GString *named_dest;
	Unicode *link_title;
	Ref page_ref;
	gint page_num = -1;

	g_return_val_if_fail (PDF_IS_DOCUMENT (document_bookmarks), FALSE);
	g_return_val_if_fail (iter != NULL, FALSE);
	g_return_val_if_fail (title != NULL, FALSE);
	g_return_val_if_fail (type != NULL, FALSE);
	g_return_val_if_fail (page != NULL, FALSE);

	anItem = (OutlineItem *)iter->items->get(iter->index);
	link_action = anItem->getAction ();
	link_title = anItem->getTitle ();

	if (link_action) {
		switch (link_action->getKind ()) {

		case actionGoTo:
			link_goto = dynamic_cast <LinkGoTo *> (link_action);
			link_dest = link_goto->getDest ();
			named_dest = link_goto->getNamedDest ();

			/* Wow!  This seems excessively slow on large
			 * documents. I need to investigate more... -jrb */
			if (link_dest != NULL) {
				link_dest = link_dest->copy ();
			} else if (named_dest != NULL) {
				named_dest = named_dest->copy ();
				link_dest = pdf_document->doc->findDest (named_dest);
				delete named_dest;
			}
			if (link_dest != NULL) {
				if (link_dest->isPageRef ()) {
					page_ref = link_dest->getPageRef ();
					page_num = pdf_document->doc->findPage (page_ref.num, page_ref.gen);
				} else {
					page_num = link_dest->getPageNum ();
				}

				delete link_dest;
			}

			break;
		case actionURI:
			link_uri = dynamic_cast <LinkURI *> (link_action);
			break;

		case actionNamed:
			/*Skip, for now */
		default:
			g_warning ("Unknown link action type: %d", link_action->getKind ());
		}

		*title = g_strdup (unicode_to_char (anItem, pdf_document->umap));
	} else if (link_title) {
		*title = g_strdup (unicode_to_char (anItem, pdf_document->umap));
	}

	*type = EV_DOCUMENT_BOOKMARKS_TYPE_LINK;
	*page = page_num;

	return TRUE;
}

static EvDocumentBookmarksIter *
pdf_document_bookmarks_get_child (EvDocumentBookmarks     *document_bookmarks,
				  EvDocumentBookmarksIter *bookmarks_iter)
{
	BookmarksIter *iter = (BookmarksIter *)bookmarks_iter;
	BookmarksIter *child_iter;
	OutlineItem *anItem;

	g_return_val_if_fail (PDF_IS_DOCUMENT (document_bookmarks), FALSE);

	anItem = (OutlineItem *)iter->items->get(iter->index);
	anItem->open ();
	if (! (anItem->hasKids() && anItem->getKids()) )
		return NULL;

	child_iter = g_new0 (BookmarksIter, 1);
	child_iter->index = 0;
	child_iter->level = iter->level + 1;
	child_iter->items = anItem->getKids ();
	g_assert (child_iter->items);

	return (EvDocumentBookmarksIter *) child_iter;
}

static gboolean
pdf_document_bookmarks_next (EvDocumentBookmarks     *document_bookmarks,
			     EvDocumentBookmarksIter *bookmarks_iter)
{
	BookmarksIter *iter = (BookmarksIter *) bookmarks_iter;

	g_return_val_if_fail (PDF_IS_DOCUMENT (document_bookmarks), FALSE);

	iter->index++;
	if (iter->index >= iter->items->getLength())
		return FALSE;

	return TRUE;
}

static void
pdf_document_bookmarks_free_iter (EvDocumentBookmarks     *document_bookmarks,
				  EvDocumentBookmarksIter *iter)
{
	g_return_if_fail (PDF_IS_DOCUMENT (document_bookmarks));
	g_return_if_fail (iter != NULL);

	/* FIXME: Should I close all the nodes?? Free them? */
	g_free (iter);
}

static void
pdf_document_finalize (GObject *object)
{
	PdfDocument *pdf_document = PDF_DOCUMENT (object);

	if (pdf_document->umap) {
		pdf_document->umap->decRefCnt ();
		pdf_document->umap = NULL;
	}

	if (pdf_document->search)
		pdf_document_search_free (pdf_document->search);

	if (pdf_document->target)
		g_object_unref (pdf_document->target);

	if (pdf_document->out)
		delete pdf_document->out;
	if (pdf_document->ps_out)
		delete pdf_document->ps_out;
	if (pdf_document->doc)
		delete pdf_document->doc;

}

static void
pdf_document_set_property (GObject *object,
		           guint prop_id,
		           const GValue *value,
		           GParamSpec *pspec)
{
	switch (prop_id)

	{
		case PROP_TITLE:
			/* read only */
			break;
	}
}

static gboolean
has_unicode_marker (GString *string)
{
	return ((string->getChar (0) & 0xff) == 0xfe &&
		(string->getChar (1) & 0xff) == 0xff);
}

static gchar *
pdf_info_dict_get_string (Dict *info_dict, const gchar *key) {
	Object obj;
	GString *value;
	gchar *result;

	g_return_val_if_fail (info_dict != NULL, NULL);
	g_return_val_if_fail (key != NULL, NULL);

	if (!info_dict->lookup ((gchar *)key, &obj)->isString ()) {
		obj.free ();
		return g_strdup (_("Unknown"));
	}

	value = obj.getString ();

	if (has_unicode_marker (value)) {
		result = g_convert (value->getCString () + 2,
				    value->getLength () - 2,
				    "UTF-8", "UTF-16BE", NULL, NULL, NULL);
	} else {
		result = g_strndup (value->getCString (), value->getLength ());
	}

	obj.free ();

	return result;
}

static char *
pdf_document_get_title (PdfDocument *pdf_document)
{
	char *title = NULL;
	Object info;

	pdf_document->doc->getDocInfo (&info);

	if (info.isDict ()) {
		title = pdf_info_dict_get_string (info.getDict(), "Title");
	}

	return title;
}

static void
pdf_document_get_property (GObject *object,
		           guint prop_id,
		           GValue *value,
		           GParamSpec *pspec)
{
	PdfDocument *pdf_document = PDF_DOCUMENT (object);
	char *title;

	switch (prop_id)
	{
		case PROP_TITLE:
			title = pdf_document_get_title (pdf_document);
			g_value_set_string (value, title);
			g_free (title);
			break;
	}
}

static void
pdf_document_class_init (PdfDocumentClass *klass)
{
	GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

	gobject_class->finalize = pdf_document_finalize;
	gobject_class->get_property = pdf_document_get_property;
	gobject_class->set_property = pdf_document_set_property;

	g_object_class_override_property (gobject_class, PROP_TITLE, "title");
}

static void
pdf_document_document_iface_init (EvDocumentIface *iface)
{
	iface->load = pdf_document_load;
	iface->save = pdf_document_save;
	iface->get_n_pages = pdf_document_get_n_pages;
	iface->set_page = pdf_document_set_page;
	iface->get_page = pdf_document_get_page;
	iface->set_scale = pdf_document_set_scale;
	iface->set_target = pdf_document_set_target;
	iface->set_page_offset = pdf_document_set_page_offset;
	iface->get_page_size = pdf_document_get_page_size;
	iface->render = pdf_document_render;
}

static void
pdf_document_ps_exporter_iface_init (EvPSExporterIface *iface)
{
	iface->begin = pdf_document_ps_export_begin;
	iface->do_page = pdf_document_ps_export_do_page;
	iface->end = pdf_document_ps_export_end;
}


static void
pdf_document_find_iface_init (EvDocumentFindIface *iface)
{
        iface->begin = pdf_document_find_begin;
        iface->cancel = pdf_document_find_cancel;
}

static void
pdf_document_document_bookmarks_iface_init (EvDocumentBookmarksIface *iface)
{
	iface->has_document_bookmarks = pdf_document_bookmarks_has_document_bookmarks;
	iface->begin_read = pdf_document_bookmarks_begin_read;
	iface->get_values = pdf_document_bookmarks_get_values;
	iface->get_child = pdf_document_bookmarks_get_child;
	iface->next = pdf_document_bookmarks_next;
	iface->free_iter = pdf_document_bookmarks_free_iter;
}

/* Thumbnails */
static GdkPixbuf *
pdf_document_thumbnails_get_page_pixbuf (PdfDocument *pdf_document,
					 gdouble      scale_factor,
					 gint         page_num,
					 gint         width,
					 gint         height)
{
	GdkPixmap *pixmap;
	GDKSplashOutputDev *output;
	GdkPixbuf *pixbuf;
	GdkPixbuf *shadow;

	pixmap = gdk_pixmap_new (pdf_document->target,
				 width, height, -1);

	output = new GDKSplashOutputDev (gdk_drawable_get_screen (pdf_document->target),
					 NULL, NULL);
	output->startDoc (pdf_document->doc->getXRef());
	pdf_document->doc->displayPage (output,
					page_num + 1,
					72*scale_factor,
					72*scale_factor,
					0, gTrue, gFalse);
	output->redraw (0, 0,
			pixmap,
			0, 0,
			width, height);
	pixbuf = gdk_pixbuf_get_from_drawable (NULL,
					       pixmap,
					       NULL,
					       0, 0,
					       0, 0,
					       width, height);
	gdk_drawable_unref (pixmap);
	delete output;

	shadow = ev_document_misc_get_thumbnail_frame (-1, -1, pixbuf);
	g_object_unref (pixbuf);

	return shadow;
}

static GdkPixbuf *
pdf_document_thumbnails_get_thumbnail (EvDocumentThumbnails *document_thumbnails,
				       gint 		     page,
				       gint                  width)
{
	PdfDocument *pdf_document = PDF_DOCUMENT (document_thumbnails);
	GdkPixbuf *thumbnail;
	Page *the_page;
	Object the_thumb;
	Thumb *thumb = NULL;
	gboolean have_ethumbs = FALSE;
	gdouble page_ratio;
	gint dest_height;

	/* getPage seems to want page + 1 for some reason; */
	the_page = pdf_document->doc->getCatalog ()->getPage (page + 1);
	the_page->getThumb(&the_thumb);

	page_ratio = the_page->getHeight () / the_page->getWidth ();
	dest_height = (gint) (width * page_ratio);


	if (!(the_thumb.isNull () || the_thumb.isNone())) {
		/* Build the thumbnail object */
		thumb = new Thumb(pdf_document->doc->getXRef (),
				  &the_thumb);

		have_ethumbs = thumb->ok();
	}

	if (have_ethumbs) {
		guchar *data;
		GdkPixbuf *tmp_pixbuf;

		data = thumb->getPixbufData();
		tmp_pixbuf = gdk_pixbuf_new_from_data (data,
						       GDK_COLORSPACE_RGB,
						       FALSE,
						       8,
						       thumb->getWidth (),
						       thumb->getHeight (),
						       thumb->getWidth () * 3,
						       NULL, NULL);
		/* FIXME: do we want to check that the thumb's size isn't ridiculous?? */
		thumbnail = ev_document_misc_get_thumbnail_frame (-1, -1, tmp_pixbuf);
		g_object_unref (tmp_pixbuf);
	} else {
		gdouble scale_factor;

		scale_factor = (gdouble)width / the_page->getWidth ();

		thumbnail = pdf_document_thumbnails_get_page_pixbuf (pdf_document,
								     scale_factor,
								     page,
								     width,
								     dest_height);
	}

	return thumbnail;
}
static void
pdf_document_document_thumbnails_iface_init (EvDocumentThumbnailsIface *iface)
{
	iface->get_thumbnail = pdf_document_thumbnails_get_thumbnail;
}


static void
pdf_document_init (PdfDocument *pdf_document)
{
	pdf_document->page = 1;
	pdf_document->page_x_offset = 0;
	pdf_document->page_y_offset = 0;
	pdf_document->scale = 1.;

	pdf_document->page_valid = FALSE;
}

