#include <ctype.h>
#include <sqlite3.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "bs.h"
#include "db.h"
#include "facebooc_server.h"
#include "kv.h"
#include "models/account.h"
#include "models/connection.h"
#include "models/like.h"
#include "models/post.h"
#include "models/session.h"
#include "server.h"
#include "template.h"
#include "utility.h"

// Facebooc is an directive class of Server
struct Facebooc {
	Server* server;
};

static Response* home(Request*);
static Response* dashboard(Request*);
static Response* profile(Request*);
static Response* post(Request*);
static Response* like(Request*);
static Response* unlike(Request*);
static Response* connect(Request*);
static Response* search(Request*);
static Response* login(Request*);
static Response* logout(Request*);
static Response* signup(Request*);
static Response* static_handler(Request* req);
static Response* notFound(Request*);  // default route

static inline int get_id(const char* uri);
static const Account* get_account(const Cookie* c);

Facebooc* FB_new(const uint16_t port) {
	Facebooc* s = malloc(sizeof(Facebooc));
	s->server = serverNew(port);
	serverAddHandler(s->server, "signup", signup);
	serverAddHandler(s->server, "logout", logout);
	serverAddHandler(s->server, "login", login);
	serverAddHandler(s->server, "search", search);
	serverAddHandler(s->server, "connect", connect);
	serverAddHandler(s->server, "like", like);
	serverAddHandler(s->server, "unlike", unlike);
	serverAddHandler(s->server, "post", post);
	serverAddHandler(s->server, "profile", profile);
	serverAddHandler(s->server, "dashboard", dashboard);
	serverAddHandler(s->server, "", home);
	serverAddHandler(s->server, "static", static_handler);

	set_callback(s->server, notFound);

	initDB();

	return s;
}

int FB_run(Facebooc* s) {
	serverServe(s->server);
	return 1;
}

void FB_delete(Facebooc* s) {
	if(s->server)
		serverDel(s->server);
	void db_close();
	free(s);
	s = NULL;
}

static int invalid(Template* template, const char* key, const char* value) {
	templateSet(template, key, value);
	int valid = false;
	return valid;
}

#define min(x, y) ((x) < (y) ? (x) : (y))

// compare str1 and str2 from tail
static inline int rev_cmp(const char* str1, size_t len1, const char* str2, size_t len2) {
	const long smaller = min(len1, len2);
	return strncmp(str1 + len1 - smaller, str2 + len2 - smaller, smaller);
}

static inline const char* mimeType_mux(const char* path) {
	static const char* file_type[] = {
		"html", "json", "jpeg", "jpg", "gif", "png", "css", "js",
	};
	static const size_t file_type_len = sizeof(file_type) / sizeof(*file_type);

	static const char* mime_type[] = {
		"text/html", "application/json",	   "image/jpeg", "image/jpeg", "image/gif", "image/png",
		"text/css",	 "application/javascript", "text/plain",
	};

	size_t i = 0;
	for(; i < file_type_len; i++) {
		if(rev_cmp(path, strlen(path), file_type[i], strlen(file_type[i])) == 0)
			break;
	}

	return mime_type[i];
}

static Response* static_handler(Request* req) {
	// EXIT ON SHENANIGANS
	if(strstr(req->uri, "../"))
		return NULL;

	const char* filename = req->uri + 1;

	// EXIT ON DIRS
	struct stat sbuff;
	if(stat(filename, &sbuff) < 0 || S_ISDIR(sbuff.st_mode))
		return NULL;

	// EXIT ON NOT FOUND
	FILE* file = fopen(filename, "r");
	if(!file)
		return NULL;

	// GET LENGTH
	char lens[25];
	fseek(file, 0, SEEK_END);
	size_t len = ftell(file);
	snprintf(lens, 10, "%ld", (long int)len);
	rewind(file);

	// SET BODY
	Response* response = responseNew();

	char* buff = malloc(sizeof(char) * len);
	(void)!fread(buff, sizeof(char), len, file);
	responseSetBody(response, bsNewLen(buff, len));
	fclose(file);
	free(buff);

	// MIME TYPE
	const char* mimeType = mimeType_mux(req->uri);

	// RESPOND
	responseSetStatus(response, OK);
	responseAddHeader(response, "Content-Type", mimeType);
	responseAddHeader(response, "Content-Length", lens);
	responseAddHeader(response, "Cache-Control", "max-age=2592000");
	return response;
}

// Get sid string from cookies
static const Account* get_account(const Cookie* c) {
	// TODO: Use a object pool to reduce malloc times
	if(unlikely(c == NULL))
		return NULL;
	const char* sid = Cookie_get(c, "sid");
	return accountGetBySId(get_db(), sid);
}

static Response* home(Request* req) {
	// ! What do here do?
	const Account* my_acc = get_account(req->cookies);
	if(my_acc) {
		accountDel((Account*)my_acc);
		return responseNewRedirect("/dashboard/");
	}

	Response* response = responseNew();
	Template* template = templateNew("templates/index.html");
	responseSetStatus(response, OK);
	templateSet(template, "active", "home");
	templateSet(template, "subtitle", "Home");
	responseSetBody(response, templateRender(template));
	templateDel(template);
	return response;
}

static Response* dashboard(Request* req) {
	const Account* my_acc = get_account(req->cookies);
	if(unlikely(my_acc == NULL))
		return NULL;

	Node* postCell = postGetLatestGraph(get_db(), my_acc->id, 0);
	char* res = NULL;
	if(postCell)
		res = bsNew("<ul class=\"posts\">");

	// ! What do here do?
	while(postCell) {
		Post* post = (Post*)postCell->value;
		Account* account = accountGetById(get_db(), post->authorId);
		bool liked = likeLiked(get_db(), my_acc->id, post->id);
		const size_t acc_len = strlen(account->name);
		const size_t post_len = strlen(post->body);

		char* bbuff = bsNewLen("", strlen(post->body) + 256);
		snprintf(bbuff, 86 + acc_len + post_len,
				 "<li class=\"post-item\">"
				 "<div class=\"post-author\">%s</div>"
				 "<div class=\"post-content\">"
				 "%s"
				 "</div>",
				 account->name, post->body);
		accountDel(account);
		bsLCat(&res, bbuff);

		char sbuff[128];
		if(liked) {
			snprintf(sbuff, 55, "<a class=\"btn\" href=\"/unlike/%d/\">Liked</a> - ", post->id);
			bsLCat(&res, sbuff);
		}
		else {
			snprintf(sbuff, 52, "<a class=\"btn\" href=\"/like/%d/\">Like</a> - ", post->id);
			bsLCat(&res, sbuff);
		}

		time_t t = post->createdAt;
		struct tm* info = gmtime(&t);
		info->tm_hour = info->tm_hour + 8;
		strftime(sbuff, 128, "%c GMT+8", info);
		bsLCat(&res, sbuff);
		bsLCat(&res, "</li>");

		bsDel(bbuff);
		postDel(post);
		Node* postPCell = postCell;
		postCell = postCell->next;

		free(postPCell);
	}

	Template* template = templateNew("templates/dashboard.html");
	if(res) {
		bsLCat(&res, "</ul>");
		templateSet(template, "graph", res);
		bsDel(res);
	}
	else {
		templateSet(template, "graph",
					"<ul class=\"posts\"><div class=\"not-found\">Nothing "
					"here.</div></ul>");
	}

	templateSet(template, "active", "dashboard");
	templateSet(template, "loggedIn", "t");
	templateSet(template, "subtitle", "Dashboard");
	templateSet(template, "accountName", my_acc->name);
	Response* response = responseNew();
	responseSetStatus(response, OK);
	responseSetBody(response, templateRender(template));
	templateDel(template);
	accountDel((Account*)my_acc);
	return response;
}

// Preview someone's profile page
static Response* profile(Request* req) {
	// Check I'm logined
	const Account* my_acc = get_account(req->cookies);
	if(unlikely(my_acc == NULL))
		return NULL;

	const Account* acc2 = accountGetById(get_db(), get_id(req->uri));

	if(!acc2)
		return NULL;
	if(acc2->id == my_acc->id) {
		accountDel((Account*)my_acc);
		accountDel((Account*)acc2);
		return responseNewRedirect("/dashboard/");
	}

	// VLA is not in POSIX1.
	// Max length of uid is len(INT_MAX), in other words, the maxlen of uid is 10
	char acc2_id_str[11] = { 0 };
	snprintf(acc2_id_str, 11, "%d", acc2->id);

	Connection* connection = connectionGetByAccountIds(get_db(), my_acc->id, acc2->id);
	char connectStr[512] = { 0 };
	if(connection) {
		snprintf(connectStr, 26 + strlen(acc2->name), "You and %s are connected!", acc2->name);
	}
	else {
		const size_t name_len = strlen(acc2->name);
		snprintf(connectStr, 76 + name_len + 10,
				 "You and %s are not connected."
				 " <a href=\"/connect/%d/\">Click here</a> to connect!",
				 acc2->name, acc2->id);
	}
	connectionDel(connection);

	Node* postPCell = NULL;
	Node* postCell = postGetLatest(get_db(), acc2->id, 0);

	char* res = NULL;
	if(postCell)
		res = bsNew("<ul class=\"posts\">");
	bool liked;
	char sbuff[128];
	char* bbuff = NULL;
	time_t t;
	while(postCell) {
		Post* post = (Post*)postCell->value;
		liked = likeLiked(get_db(), my_acc->id, post->id);

		const size_t body_len = strlen(post->body);
		bbuff = bsNewLen("", body_len + 256);
		snprintf(bbuff, 54 + body_len,
				 "<li class=\"post-item\"><div class=\"post-author\">%s</div>", post->body);
		bsLCat(&res, bbuff);

		if(liked) {
			bsLCat(&res, "Liked - ");
		}
		else {
			snprintf(sbuff, 52, "<a class=\"btn\" href=\"/like/%d/\">Like</a> - ", post->id);
			bsLCat(&res, sbuff);
		}

		t = post->createdAt;
		strftime(sbuff, 128, "%c GMT", gmtime(&t));
		bsLCat(&res, sbuff);
		bsLCat(&res, "</li>");

		bsDel(bbuff);
		postDel(post);
		postPCell = postCell;
		postCell = (Node*)postCell->next;

		free(postPCell);
	}
	Template* template = templateNew("templates/profile.html");
	if(res) {
		bsLCat(&res, "</ul>");
		templateSet(template, "profilePosts", res);
		bsDel(res);
	}
	else {
		templateSet(template, "profilePosts",
					"<h4 class=\"not-found\">This person has not posted "
					"anything yet!</h4>");
	}

	templateSet(template, "active", "profile");
	templateSet(template, "loggedIn", "t");
	templateSet(template, "subtitle", acc2->name);
	templateSet(template, "profileId", acc2_id_str);
	templateSet(template, "profileName", acc2->name);
	templateSet(template, "profileEmail", acc2->email);
	templateSet(template, "profileConnect", connectStr);
	templateSet(template, "accountName", my_acc->name);
	Response* response = responseNew();
	responseSetStatus(response, OK);
	responseSetBody(response, templateRender(template));
	accountDel((Account*)my_acc);
	accountDel((Account*)acc2);
	templateDel(template);
	return response;
}

// I post a post via HTTP POST
static Response* post(Request* req) {
	const Account* acc = get_account(req->cookies);
	if(unlikely(acc == NULL))
		goto fail;

	if(unlikely(req->method != POST))
		goto fail;

	const char* postStr = kvFindList(req->postBody, "post");

	if(strlen(postStr) == 0)
		goto fail;
	else if(strlen(postStr) < MAX_BODY_LEN)
		postDel(postCreate(get_db(), acc->id, postStr));

fail:
	accountDel((Account*)acc);
	return responseNewRedirect("/dashboard/");
}

static Response* unlike(Request* req) {
	const Account* my_acc = get_account(req->cookies);
	char redir_to[32] = { "/dashboard/" };
	if(unlikely(my_acc == NULL))
		goto fail;

	const Post* post = postGetById(get_db(), get_id(req->uri));
	if(!post)
		goto fail;

	likeDel(likeDelete(get_db(), my_acc->id, post->authorId, post->id));

	if(kvFindList(req->queryString, "r"))
		snprintf(redir_to, 32, "/profile/%d/", post->authorId);

fail:
	accountDel((Account*)my_acc);
	return responseNewRedirect(redir_to);
}

static Response* like(Request* req) {
	const Account* my_acc = get_account(req->cookies);
	char redir_to[32] = { "/dashboard/" };
	if(unlikely(my_acc == NULL))
		goto fail;

	const Post* post = postGetById(get_db(), get_id(req->uri));
	if(!post)
		goto fail;

	likeDel(likeCreate(get_db(), my_acc->id, post->authorId, post->id));

	if(kvFindList(req->queryString, "r"))
		snprintf(redir_to, 32, "/profile/%d/", post->authorId);

fail:
	accountDel((Account*)my_acc);
	return responseNewRedirect(redir_to);
}

static Response* connect(Request* req) {
	const Account* my_acc = get_account(req->cookies);
	char redir_to[32] = { "/dashboard/" };
	if(unlikely(my_acc == NULL))
		goto fail;

	const Account* account = accountGetById(get_db(), get_id(req->uri));
	if(!account)
		goto fail;

	connectionDel(connectionCreate(get_db(), my_acc->id, account->id));

	snprintf(redir_to, 32, "/profile/%d/", account->id);

fail:
	accountDel((Account*)my_acc);
	return responseNewRedirect(redir_to);
}

static Response* search(Request* req) {
	const Account* my_acc = get_account(req->cookies);
	if(unlikely(my_acc == NULL))
		return responseNewRedirect("/login/");
	accountDel((Account*)my_acc);

	char* query = kvFindList(req->queryString, "q");

	if(!query)
		return NULL;

	char* res = NULL;
	char sbuff[1024];

	Node* accountCell = accountSearch(get_db(), query, 0);
	if(accountCell)
		res = bsNew("<ul class=\"search-results\">");

	Account* account = NULL;
	Node* accountPCell = NULL;

	while(accountCell) {
		account = (Account*)accountCell->value;

		const size_t name_len = strlen(account->name);
		const size_t email_len = strlen(account->email);

		snprintf(sbuff, 62 + name_len + email_len,
				 "<li><a href=\"/profile/%d/\">%s</a> (<span>%s</span>)</li>\n", account->id,
				 account->name, account->email);
		bsLCat(&res, sbuff);

		accountDel(account);
		accountPCell = accountCell;
		accountCell = (Node*)accountCell->next;

		free(accountPCell);
	}

	if(res)
		bsLCat(&res, "</ul>");

	Response* response = responseNew();
	Template* template = templateNew("templates/search.html");
	responseSetStatus(response, OK);

	if(!res) {
		templateSet(template, "results",
					"<h4 class=\"not-found\">There were no results "
					"for your query.</h4>");
	}
	else {
		templateSet(template, "results", res);
		bsDel(res);
	}

	templateSet(template, "searchQuery", query);
	templateSet(template, "active", "search");
	templateSet(template, "loggedIn", "t");
	templateSet(template, "subtitle", "Search");
	responseSetBody(response, templateRender(template));
	templateDel(template);
	return response;
}

static Response* login(Request* req) {
	const Account* my_acc = get_account(req->cookies);
	if(unlikely(my_acc != NULL)) {	// It's not usually logined
		accountDel((Account*)my_acc);
		return responseNewRedirect("/dashboard/");
	}

	Response* response = responseNew();
	Template* template = templateNew("templates/login.html");
	responseSetStatus(response, OK);
	templateSet(template, "active", "login");
	templateSet(template, "subtitle", "Login");

	if(req->method == POST) {
		char* username = kvFindList(req->postBody, "username");
		char* password = kvFindList(req->postBody, "password");

		if(!username) {
			invalid(template, "usernameError", "Username missing!");
		}
		else {
			templateSet(template, "formUsername", username);
		}

		if(!password) {
			invalid(template, "passwordError", "Password missing!");
		}

		bool valid = account_auth(get_db(), username, password);

		if(valid) {
			Session* session = sessionCreate(get_db(), username, password);
			if(session) {
				responseSetStatus(response, FOUND);
				responseAddCookie(response, "sid", session->sessionId, NULL, NULL, 3600 * 24 * 30);
				responseAddHeader(response, "Location", "/dashboard/");
				templateDel(template);
				sessionDel(session);
				return response;
			}
			else {
				invalid(template, "usernameError", "Invalid username or password.");
			}
		}
		else {
			invalid(template, "usernameError", "Invalid username or password.");
		}
	}

	responseSetBody(response, templateRender(template));
	templateDel(template);
	return response;
}

static Response* logout(Request* req) {
	const Account* my_acc = get_account(req->cookies);
	if(unlikely(my_acc == NULL)) {	// It's usually logined
		accountDel((Account*)my_acc);
		return responseNewRedirect("/");
	}

	Response* response = responseNewRedirect("/");
	// Reset cookie
	responseAddCookie(response, "sid", "", NULL, NULL, -1);
	return response;
}

static Response* signup(Request* req) {
	const Account* my_acc = get_account(req->cookies);
	if(unlikely(my_acc != NULL)) {
		// If you already logined, you should not regist
		accountDel((Account*)my_acc);
		return responseNewRedirect("/dashboard/");
	}

	Response* response = responseNew();
	Template* template = templateNew("templates/signup.html");
	templateSet(template, "active", "signup");
	templateSet(template, "subtitle", "Sign Up");
	responseSetStatus(response, OK);

	if(req->method == POST) {
		bool valid = true;
		const char* name = kvFindList(req->postBody, "name");
		const char* email = kvFindList(req->postBody, "email");
		const char* username = kvFindList(req->postBody, "username");
		const char* password = kvFindList(req->postBody, "password");
		const char* confirmPassword = kvFindList(req->postBody, "confirm-password");

		if(!name) {
			invalid(template, "nameError", "You must enter your name!");
		}
		else if(strlen(name) < 5 || strlen(name) > 50) {
			invalid(template, "nameError", "Your name must be between 5 and 50 characters long.");
		}
		else {
			templateSet(template, "formName", name);
		}

		if(!email) {
			invalid(template, "emailError", "You must enter an email!");
		}
		else if(strchr(email, '@') == NULL) {
			invalid(template, "emailError", "Invalid email.");
		}
		else if(strlen(email) < 3 || strlen(email) > 50) {
			invalid(template, "emailError", "Your email must be between 3 and 50 characters long.");
		}
		else if(!accountCheckEmail(get_db(), email)) {
			invalid(template, "emailError", "This email is taken.");
		}
		else {
			templateSet(template, "formEmail", email);
		}

		if(!username) {
			invalid(template, "usernameError", "You must enter a username!");
		}
		else if(strlen(username) < 3 || strlen(username) > 50) {
			invalid(template, "usernameError",
					"Your username must be between 3 and 50 characters long.");
		}
		else if(!accountCheckUsername(get_db(), username)) {
			invalid(template, "usernameError", "This username is taken.");
		}
		else {
			templateSet(template, "formUsername", username);
		}

		if(!password) {
			invalid(template, "passwordError", "You must enter a password!");
		}
		else if(strlen(password) < 8) {
			invalid(template, "passwordError", "Your password must be at least 8 characters long!");
		}

		if(!confirmPassword) {
			invalid(template, "confirmPasswordError", "You must confirm your password.");
		}
		else if(strcmp(password, confirmPassword) != 0) {
			invalid(template, "confirmPasswordError", "The two passwords must be the same.");
		}

		if(valid) {
			Account* account = accountCreate(get_db(), name, email, username, password);

			if(account) {
				responseSetStatus(response, FOUND);
				responseAddHeader(response, "Location", "/login/");
				templateDel(template);
				accountDel(account);
				return response;
			}
			else {
				invalid(template, "nameError", "Unexpected error. Please try again later.");
			}
		}
	}

	responseSetBody(response, templateRender(template));
	templateDel(template);
	return response;
}

static Response* notFound(Request* req) {
	const Account* my_acc = get_account(req->cookies);
	const int is_loggedIn = !!my_acc;
	accountDel((Account*)my_acc);

	Response* response = responseNew();
	Template* template = templateNew("templates/404.html");
	templateSet(template, "loggedIn", is_loggedIn ? "t" : "");
	templateSet(template, "subtitle", "404 Not Found");
	responseSetStatus(response, NOT_FOUND);
	responseSetBody(response, templateRender(template));
	templateDel(template);
	return response;
}

static inline int get_id(const char* uri) {
	// format: "/%s/<id>/"

	char* begin = strchr(uri + 1, '/') + 1;
	char* end = strchr(begin, '/');
	const size_t len = end - begin;

	char new_str[10] = { 0 };
	memcpy(new_str, begin, len);

	return atoi(new_str);
}
