/*-*- Mode: C; c-basic-offset: 8; indent-tabs-mode: nil -*-*/

/***
  This file is part of systemd.

  Copyright 2014 Tom Gundersen <teg@jklm.no>

  systemd is free software; you can redistribute it and/or modify it
  under the terms of the GNU Lesser General Public License as published by
  the Free Software Foundation; either version 2.1 of the License, or
  (at your option) any later version.

  systemd is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with systemd; If not, see <http://www.gnu.org/licenses/>.
 ***/

#include <resolv.h>

#include "alloc-util.h"
#include "dns-domain.h"
#include "fd-util.h"
#include "fileio-label.h"
#include "fileio.h"
#include "ordered-set.h"
#include "resolved-conf.h"
#include "resolved-resolv-conf.h"
#include "string-util.h"
#include "strv.h"

int manager_read_resolv_conf(Manager *m) {
        _cleanup_fclose_ FILE *f = NULL;
        struct stat st, own;
        char line[LINE_MAX];
        usec_t t;
        int r;

        assert(m);

        /* Reads the system /etc/resolv.conf, if it exists and is not
         * symlinked to our own resolv.conf instance */

        if (!m->read_resolv_conf)
                return 0;

        r = stat("/etc/resolv.conf", &st);
        if (r < 0) {
                if (errno == ENOENT)
                        return 0;

                r = log_warning_errno(errno, "Failed to stat /etc/resolv.conf: %m");
                goto clear;
        }

        /* Have we already seen the file? */
        t = timespec_load(&st.st_mtim);
        if (t == m->resolv_conf_mtime)
                return 0;

        /* Is it symlinked to our own file? */
        if (stat("/run/systemd/resolve/resolv.conf", &own) >= 0 &&
            st.st_dev == own.st_dev &&
            st.st_ino == own.st_ino)
                return 0;

        f = fopen("/etc/resolv.conf", "re");
        if (!f) {
                if (errno == ENOENT)
                        return 0;

                r = log_warning_errno(errno, "Failed to open /etc/resolv.conf: %m");
                goto clear;
        }

        if (fstat(fileno(f), &st) < 0) {
                r = log_error_errno(errno, "Failed to stat open file: %m");
                goto clear;
        }

        dns_server_mark_all(m->dns_servers);
        dns_search_domain_mark_all(m->search_domains);

        FOREACH_LINE(line, f, r = -errno; goto clear) {
                const char *a;
                char *l;

                l = strstrip(line);
                if (*l == '#' || *l == ';')
                        continue;

                a = first_word(l, "nameserver");
                if (a) {
                        r = manager_add_dns_server_by_string(m, DNS_SERVER_SYSTEM, a);
                        if (r < 0)
                                log_warning_errno(r, "Failed to parse DNS server address '%s', ignoring.", a);

                        continue;
                }

                a = first_word(l, "domain");
                if (!a) /* We treat "domain" lines, and "search" lines as equivalent, and add both to our list. */
                        a = first_word(l, "search");
                if (a) {
                        r = manager_parse_search_domains_and_warn(m, a);
                        if (r < 0)
                                log_warning_errno(r, "Failed to parse search domain string '%s', ignoring.", a);
                }
        }

        m->resolv_conf_mtime = t;

        /* Flush out all servers and search domains that are still
         * marked. Those are then ones that didn't appear in the new
         * /etc/resolv.conf */
        dns_server_unlink_marked(m->dns_servers);
        dns_search_domain_unlink_marked(m->search_domains);

        /* Whenever /etc/resolv.conf changes, start using the first
         * DNS server of it. This is useful to deal with broken
         * network managing implementations (like NetworkManager),
         * that when connecting to a VPN place both the VPN DNS
         * servers and the local ones in /etc/resolv.conf. Without
         * resetting the DNS server to use back to the first entry we
         * will continue to use the local one thus being unable to
         * resolve VPN domains. */
        manager_set_dns_server(m, m->dns_servers);

        /* Unconditionally flush the cache when /etc/resolv.conf is
         * modified, even if the data it contained was completely
         * identical to the previous version we used. We do this
         * because altering /etc/resolv.conf is typically done when
         * the network configuration changes, and that should be
         * enough to flush the global unicast DNS cache. */
        if (m->unicast_scope)
                dns_cache_flush(&m->unicast_scope->cache);

        return 0;

clear:
        dns_server_unlink_all(m->dns_servers);
        dns_search_domain_unlink_all(m->search_domains);
        return r;
}

static void write_resolv_conf_server(DnsServer *s, FILE *f, unsigned *count) {
        assert(s);
        assert(f);
        assert(count);

        (void) dns_server_string(s);

        if (!s->server_string) {
                log_warning("Our of memory, or invalid DNS address. Ignoring server.");
                return;
        }

        if (*count == MAXNS)
                fputs("# Too many DNS servers configured, the following entries may be ignored.\n", f);
        (*count) ++;

        fprintf(f, "nameserver %s\n", s->server_string);
}

static void write_resolv_conf_search(
                const char *domain,
                FILE *f,
                unsigned *count,
                unsigned *length) {

        assert(domain);
        assert(f);
        assert(length);

        if (*count >= MAXDNSRCH ||
            *length + strlen(domain) > 256) {
                if (*count == MAXDNSRCH)
                        fputs(" # Too many search domains configured, remaining ones ignored.", f);
                if (*length <= 256)
                        fputs(" # Total length of all search domains is too long, remaining ones ignored.", f);

                return;
        }

        (*length) += strlen(domain);
        (*count) ++;

        fputc(' ', f);
        fputs(domain, f);
}

static int write_resolv_conf_contents(FILE *f, OrderedSet *dns, OrderedSet *domains) {
        Iterator i;

        fputs("# This file is managed by systemd-resolved(8). Do not edit.\n#\n"
              "# Third party programs must not access this file directly, but\n"
              "# only through the symlink at /etc/resolv.conf. To manage\n"
              "# resolv.conf(5) in a different way, replace the symlink by a\n"
              "# static file or a different symlink.\n\n", f);

        if (ordered_set_isempty(dns))
                fputs("# No DNS servers known.\n", f);
        else {
                unsigned count = 0;
                DnsServer *s;

                ORDERED_SET_FOREACH(s, dns, i)
                        write_resolv_conf_server(s, f, &count);
        }

        if (!ordered_set_isempty(domains)) {
                unsigned length = 0, count = 0;
                char *domain;

                fputs("search", f);
                ORDERED_SET_FOREACH(domain, domains, i)
                        write_resolv_conf_search(domain, f, &count, &length);
                fputs("\n", f);
        }

        return fflush_and_check(f);
}

int manager_write_resolv_conf(Manager *m) {

        #define PRIVATE_RESOLV_CONF "/run/systemd/resolve/resolv.conf"

        _cleanup_ordered_set_free_ OrderedSet *dns = NULL, *domains = NULL;
        _cleanup_free_ char *temp_path = NULL;
        _cleanup_fclose_ FILE *f = NULL;
        int r;

        assert(m);

        /* Read the system /etc/resolv.conf first */
        manager_read_resolv_conf(m);

        /* Add the full list to a set, to filter out duplicates */
        r = manager_compile_dns_servers(m, &dns);
        if (r < 0)
                return r;

        r = manager_compile_search_domains(m, &domains);
        if (r < 0)
                return r;

        r = fopen_temporary_label(PRIVATE_RESOLV_CONF, PRIVATE_RESOLV_CONF, &f, &temp_path);
        if (r < 0)
                return r;

        fchmod(fileno(f), 0644);

        r = write_resolv_conf_contents(f, dns, domains);
        if (r < 0)
                goto fail;

        if (rename(temp_path, PRIVATE_RESOLV_CONF) < 0) {
                r = -errno;
                goto fail;
        }

        return 0;

fail:
        (void) unlink(PRIVATE_RESOLV_CONF);
        (void) unlink(temp_path);
        return r;
}
