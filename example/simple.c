#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../jsmn.h"

#include <sophia.h>

static int jsoneq(const char *json, jsmntok_t *tok, const char *s) {
	if (tok->type == JSMN_STRING && (int) strlen(s) == tok->end - tok->start &&
			strncmp(json + tok->start, s, tok->end - tok->start) == 0) {
		return 0;
	}
	return -1;
}

int main() {
	int i;
	int r;
	jsmn_parser p;
	jsmntok_t t[100000]; /* We expect no more than 128 tokens */

	FILE *fptr;
	fptr = fopen("TempPrep.json", "r");

	if(fptr == NULL) 
	{
		printf("No abierto");
		exit(1);
	}

	char buf[1000000];
	size_t nread = fread(buf, sizeof(char), 1000000, fptr);

    if (nread == 0) 
        fputs("Error reading file", stderr);

	fclose(fptr);

	jsmn_init(&p);
	r = jsmn_parse(&p, buf, strlen(buf), t, 100000);
	if (r < 0) {
		printf("Failed to parse JSON: %d\n", r);
		return 1;
	}

	/* Assume the top-level element is an array */
	if (r < 1 || t[0].type != JSMN_ARRAY) {
		printf("Array expected\n");
		return 1;
	}

	/* open or create environment and database */
	void *env = sp_env();
	sp_setstring(env, "sophia.path", "_test", 0);
	sp_setstring(env, "db", "test", 0);
	void *db = sp_getobject(env, "db.test");
	int rc = sp_open(env);
	if (rc == -1)
		goto error;

	/* Loop over all keys of the root object */
	char d[50], m[50], y[50], med[50], max[50], min[50], pcp[50];
	for (i = 0; i < r; i++) {
		if (t[i+1].type == JSMN_OBJECT) {
			for (int j = 0; j < t[i+1].size*2;j++) {
				jsmntok_t *g = &t[i+j+2];
				jsmntok_t *h = &t[i+j+3];
				if (jsoneq(buf, g, "Dia") == 0) {
					sprintf (d, "%.*s",h->end-h->start, (char*) buf + h->start);
					j++;
				} else if (jsoneq(buf, g, "Mes") == 0) {
					sprintf (m, "%.*s",h->end-h->start, (char*) buf + h->start);
					j++;
				} else if (jsoneq(buf, g, "Año") == 0) {
					sprintf (y, "%.*s",h->end-h->start, (char*) buf + h->start);
					j++;
				} else if (jsoneq(buf, g, "Media") == 0) {
					sprintf (med, "%.*s",h->end-h->start, (char*) buf + h->start);
					j++;
				} else if (jsoneq(buf, g, "Maxima") == 0) {
					sprintf (max, "%.*s",h->end-h->start, (char*) buf + h->start);
					j++;
				} else if (jsoneq(buf, g, "Minima") == 0) {
					sprintf (min, "%.*s",h->end-h->start, (char*) buf + h->start);
					j++;
				} else if (jsoneq(buf, g, "PCP") == 0) {
					sprintf (pcp, "%.*s",h->end-h->start, (char*) buf + h->start);
					j++;
				} else {
					printf("Unexpected key: %.*s\n", h->end-h->start, buf + h->start);
				}
			}

			char chop1[50] = ",", chop2[50] = ",", chop3[50] = ","; //Separator values
			// set
			strcat(d,strcat(m,y)); //Key
			strcat(med,strcat(chop1,strcat(max,strcat(chop2,strcat(min,strcat(chop3,pcp)))))); //Value
			
			void *o = sp_document(db);
			sp_setstring(o, "key", &d, sizeof(d));
			sp_setstring(o, "value", &med, sizeof(med));
			rc = sp_set(db, o);
			if (rc == -1)
				goto error;
			else
				printf("insertado (%s,%s)\n", d, med);

			i += t[i+1].size + 1;
		} 
	}

	/* create cursor and do forward iteration */
	void *cursor = sp_cursor(env);
	void *o = sp_document(db);
	while ((o = sp_get(cursor, o))) {
		char *key = (char*)sp_getstring(o, "key", NULL);
		char *value = (char*)sp_getstring(o, "value", NULL);

		printf("(%s, %s)\n", key, value);

		/* delete */
		void *b = sp_document(db);
		sp_setstring(b, "key", &key, sizeof(key));
		rc = sp_delete(db, b);
		if (rc != 0)
			goto error;
		else
			printf("borrado (%s,%s)\n", key, value);
	}

	

	sp_destroy(cursor);
	
	/* finish work */
	sp_destroy(env);
	return 0;

error:;
	int size;
	char *error = sp_getstring(env, "sophia.error", &size);
	printf("error: %s\n", error);
	free(error);
	sp_destroy(env);
	return 1;
}
