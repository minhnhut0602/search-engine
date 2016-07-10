#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>

#include "yajl/yajl_tree.h"
#include "indexer.h"
#include "config.h"

#undef NDEBUG
#include <assert.h>

static void        *term_index = NULL;
static math_index_t math_index = NULL;
static keyval_db_t  offset_db  = NULL;
static blob_index_t blob_index_url = NULL;
static blob_index_t blob_index_txt = NULL;

static doc_id_t   prev_docID /* docID just indexed */ = 0;
static position_t cur_position = 0;

void indexer_assign(struct indices *indices)
{
	term_index = indices->ti;
	math_index = indices->mi;
	offset_db  = indices->ofs_db;
	blob_index_url = indices->url_bi;
	blob_index_txt = indices->txt_bi;
}

static void
index_blob(blob_index_t bi, const char *str, size_t str_sz, bool compress)
{
	struct codec codec = {CODEC_GZ, NULL};
	size_t compressed_sz;
	void  *compressed;

#ifdef DEBUG_INDEXER
	printf("indexing blob:\n""%s\n", str);
#endif

	if (compress) {
		compressed_sz = codec_compress(&codec, str, str_sz, &compressed);

#ifdef DEBUG_INDEXER
		printf("compressed from %lu into %lu bytes.\n", str_sz, compressed_sz);
#endif
		blob_index_write(bi, prev_docID + 1, compressed, compressed_sz);
		free(compressed);
	} else {
#ifdef DEBUG_INDEXER
		printf("not compressed.\n");
#endif

		blob_index_write(bi, prev_docID + 1, str, str_sz);
	}
}

static bool save_offset(uint32_t offset, uint32_t n_bytes)
{
	offsetmap_from_t from;
	offsetmap_to_t   to;

	from.docID = prev_docID + 1;
	from.pos   = cur_position;
	to.offset  = offset;
	to.n_bytes = n_bytes;

#ifdef DEBUG_INDEXER
	printf("saving offset map: docID=%u, pos=%u => offset=%u, sz=%u ...\n",
	       prev_docID + 1, cur_position, offset, n_bytes);
#endif

	if(keyval_db_put(offset_db, &from, sizeof(offsetmap_from_t),
	                 &to, sizeof(offsetmap_to_t))) {
		printf("put error: %s\n", keyval_db_last_err(offset_db));
		return 1;
	}

	return 0;
}

static void index_tex(char *tex, uint32_t offset, size_t n_bytes)
{
	struct tex_parse_ret parse_ret;
	parse_ret = tex_parse(tex, 0, false);

	if (parse_ret.code == PARSER_RETCODE_SUCC) {
#ifdef DEBUG_INDEXER
		printf("[index tex] `%s'\n", tex);
#endif

		/* actual tex indexing */
		math_index_add_tex(math_index, prev_docID + 1, cur_position,
		                   parse_ret.subpaths);
		subpaths_release(&parse_ret.subpaths);

	} else {
		printf("parsing TeX (`%s') error: %s\n", tex, parse_ret.msg);
	}

	/* save offset before cur_position increases */
	save_offset(offset, n_bytes);

	/* increment position */
	cur_position ++;
}

static void index_term(char *term, uint32_t offset, size_t n_bytes)
{
#ifdef DEBUG_INDEXER
	/* print */
	printf("[index term] %s <%u, %lu>\n", term,
	       offset, n_bytes);
#endif

	/* add term into inverted-index */
	term_index_doc_add(term_index, term);

	/* save offset before cur_position increases */
	save_offset(offset, n_bytes);

	/* increment position */
	cur_position ++;
}

static LIST_IT_CALLBK(_index_term)
{
	LIST_OBJ(struct text_seg, seg, ln);

	/* adjust offset relatively to file */
	P_CAST(slice_offset, uint32_t, pa_extra);
	seg->offset += *slice_offset;

	index_term(seg->str, seg->offset, seg->n_bytes);

	LIST_GO_OVER;
}

LIST_DEF_FREE_FUN(txt_seg_li_release, struct text_seg,
                  ln, free(p));

void indexer_handle_slice(struct lex_slice *slice)
{
	size_t str_sz = strlen(slice->mb_str);
	list   li     = LIST_NULL;

#ifdef DEBUG_INDEXER
	printf("input slice: [%s]\n", slice->mb_str);
#endif

	switch (slice->type) {
	case LEX_SLICE_TYPE_MATH:
#ifdef DEBUG_INDEXER
		printf("[index math tag] %s <%u, %lu>\n", slice->mb_str,
		       slice->offset, str_sz);
#endif
		/* term_index_doc_add() is invoked here to make position numbers
		 * synchronous in both math-index and Indri. */
		term_index_doc_add(term_index, "math_exp");

		/* extract tex from math tag and add it into math-index */
		strip_math_tag(slice->mb_str, str_sz);
		index_tex(slice->mb_str, slice->offset, str_sz);

		break;

	case LEX_SLICE_TYPE_TEXT:
		eng_to_lower_case(slice->mb_str, str_sz);

		li = text_segment(slice->mb_str);
		list_foreach(&li, &_index_term, &slice->offset);
		txt_seg_li_release(&li);

		break;

	case LEX_SLICE_TYPE_ENG_TEXT:
		eng_to_lower_case(slice->mb_str, str_sz);

		index_term(slice->mb_str, slice->offset, str_sz);
		break;

	default:
		assert(0);
	}
}

static void index_maintain()
{
	if (term_index_maintain(term_index)) {
		printf("\r[index maintaining...]");
		fflush(stdout);
		sleep(2);

		keyval_db_flush(offset_db);
	}
}

static bool get_json_val(const char *json, const char **path, char *val)
{
	yajl_val tr, node;
	char err_str[1024] = {0};
	char *v;

	tr = yajl_tree_parse(json, err_str, sizeof(err_str));

	if (tr == NULL) {
		fprintf(stderr, "parser error: %s\n", err_str);
		return 0;
	}

	node = yajl_tree_get(tr, path, yajl_t_string);

	if (node == NULL) {
		fprintf(stderr, "JSON node not found.\n");
		return 0;
	}

	v = YAJL_GET_STRING(node);
	strcpy(val, v);

	yajl_tree_free(tr);
	return 1;
}

static void index_text_field(const char *txt, text_lexer lex)
{
	doc_id_t docID;
	size_t txt_sz = strlen(txt);
	FILE *fh_txt = fmemopen((void *)txt, txt_sz, "r");

	/* safe check */
	if (fh_txt == NULL) {
		perror("fmemopen() function.");
		exit(EXIT_FAILURE);
	}

	/* prepare indexing a document */
	term_index_doc_begin(term_index);

	/* invoke lexer */
	(*lex)(fh_txt);

	/* index text blob */
	index_blob(blob_index_txt, txt, txt_sz, 1);

	/* close memory file handler */
	fclose(fh_txt);

	/* done indexing this document */
	docID = term_index_doc_end(term_index);
	assert(docID == prev_docID + 1);

	/* update document indexing variables */
	prev_docID = docID;
	cur_position = 0;
}

void indexer_index_json(FILE *fh, text_lexer lex)
{
	const char *url_path[] = {"url", NULL};
	const char *txt_path[] = {"text", NULL};
	static char doc_json[MAX_CORPUS_FILE_SZ];
	static char url_field[MAX_CORPUS_FILE_SZ];
	static char txt_field[MAX_CORPUS_FILE_SZ];
	size_t      rd_sz;

	rd_sz = fread(doc_json, 1, MAX_CORPUS_FILE_SZ, fh);

	if (rd_sz == MAX_CORPUS_FILE_SZ) {
		fprintf(stderr, "corpus file too large!\n");
		return;
	}

	if (!get_json_val(doc_json, url_path, url_field))
		return;

	if (!get_json_val(doc_json, txt_path, txt_field))
		return;

	/* URL blob indexing, it is done prior than text indexing
	 * because prev_docID is not updated at this point. */
	index_blob(blob_index_url, url_field, strlen(url_field), 0);

	/* text indexing */
	index_text_field(txt_field, lex);

	/* maintain index (e.g. optimize, merge...) */
	index_maintain();
}
