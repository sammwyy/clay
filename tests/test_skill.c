#include "clay/skill.h"

#include "clay/str.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void write_file(const char *path, const char *content) {
    FILE *f = fopen(path, "w");
    assert(f);
    fputs(content, f);
    fclose(f);
}

int main(void) {
    char template[] = "/tmp/clay-skill-XXXXXX";
    char *home = mkdtemp(template);
    assert(home);
    assert(setenv("HOME", home, 1) == 0);

    char source_template[] = "/tmp/clay-skill-src-XXXXXX";
    char *source_dir = mkdtemp(source_template);
    assert(source_dir);
    ClayStr skill_md;
    clay_str_init(&skill_md);
    clay_str_printf(&skill_md, "%s/SKILL.md", source_dir);
    write_file(skill_md.data,
              "---\n"
              "name: demo-skill\n"
              "description: A demo skill for tests.\n"
              "---\n"
              "Body line one.\nBody line two.\n");

    assert(clay_skill_valid_name("demo-skill"));
    assert(!clay_skill_valid_name(""));
    assert(!clay_skill_valid_name("Has-Upper"));
    assert(!clay_skill_valid_name("../escape"));

    assert(clay_skill_read("missing") == NULL);
    assert(clay_skill_install("/nonexistent/dir", NULL) != 0);

    /* Installing by directory (SKILL.md implied) or by direct file path
       both work, and take the name from frontmatter. */
    assert(clay_skill_install(source_dir, NULL) == 0);
    assert(clay_skill_install(skill_md.data, NULL) == 0);

    /* A relative path given at install time must still resolve after the
       cwd changes - clay_skill_read runs in a later session, not the one
       that ran the install. */
    char *original_cwd = getcwd(NULL, 0);
    assert(original_cwd);
    const char *source_base = strrchr(source_dir, '/') + 1;
    assert(chdir("/tmp") == 0);
    assert(clay_skill_install(source_base, "relative-skill") == 0);
    assert(chdir(original_cwd) == 0);
    free(original_cwd);
    char *relative_content = clay_skill_read("relative-skill");
    assert(relative_content);
    assert(strstr(relative_content, source_dir));
    free(relative_content);
    assert(clay_skill_remove("relative-skill") == 0);

    char *content = clay_skill_read("demo-skill");
    assert(content);
    assert(strstr(content, source_dir));
    assert(strstr(content, "Body line one."));
    assert(!strstr(content, "description:"));
    free(content);

    char *index = clay_skill_index();
    assert(strstr(index, "demo-skill: A demo skill for tests."));
    free(index);

    ClayArray entries;
    clay_skill_list(&entries);
    assert(entries.count == 1);
    ClaySkillEntry *entry = clay_array_get(&entries, 0);
    assert(strcmp(entry->name, "demo-skill") == 0);
    assert(entry->enabled);
    clay_skill_entries_free(&entries);

    /* A name override wins over the frontmatter's own name. */
    assert(clay_skill_install(source_dir, "renamed-skill") == 0);
    assert(clay_skill_read("renamed-skill") != NULL);

    assert(clay_skill_set_enabled("demo-skill", 0) == 0);
    assert(clay_skill_read("demo-skill") == NULL);
    index = clay_skill_index();
    assert(!strstr(index, "demo-skill:"));
    assert(strstr(index, "renamed-skill:"));
    free(index);
    assert(clay_skill_set_enabled("missing-skill", 1) != 0);

    assert(clay_skill_remove("demo-skill") == 0);
    assert(clay_skill_remove("demo-skill") == 0); /* removing twice is a no-op */
    assert(clay_skill_remove("renamed-skill") == 0);
    index = clay_skill_index();
    assert(strcmp(index, "") == 0);
    free(index);

    /* A git URL is a real `git clone` (git-shelled-out, same as
       clay/checkpoint.h), not just a stored path - verify a .git directory
       actually lands under ~/.clay/skills/sources, and that reinstalling
       runs `git pull` instead of failing on an already-cloned dir. Runs
       under its own HOME since the clone isn't torn down afterward, same
       convention test_checkpoint.c's git fixtures use. */
    char git_home_template[] = "/tmp/clay-skill-git-home-XXXXXX";
    char *git_home = mkdtemp(git_home_template);
    assert(git_home);
    char git_repo_template[] = "/tmp/clay-skill-git-repo-XXXXXX";
    char *git_repo = mkdtemp(git_repo_template);
    assert(git_repo);

    ClayStr git_skill_md;
    clay_str_init(&git_skill_md);
    clay_str_printf(&git_skill_md, "%s/SKILL.md", git_repo);
    write_file(git_skill_md.data,
              "---\n"
              "name: git-skill\n"
              "description: Installed via a real git clone.\n"
              "---\n"
              "Cloned body.\n");
    clay_str_free(&git_skill_md);

    ClayStr git_setup;
    clay_str_init(&git_setup);
    clay_str_printf(&git_setup,
                    "git -C %s init -q && "
                    "git -C %s -c user.name=t -c user.email=t@t.com add -A && "
                    "git -C %s -c user.name=t -c user.email=t@t.com commit -q -m init",
                    git_repo, git_repo, git_repo);
    assert(system(git_setup.data) == 0);
    clay_str_free(&git_setup);

    assert(setenv("HOME", git_home, 1) == 0);
    ClayStr git_url;
    clay_str_init(&git_url);
    clay_str_printf(&git_url, "file://%s", git_repo);

    assert(clay_skill_install(git_url.data, NULL) == 0);
    char *git_content = clay_skill_read("git-skill");
    assert(git_content);
    assert(strstr(git_content, "Cloned body."));
    free(git_content);

    ClayArray git_entries;
    clay_skill_list(&git_entries);
    ClaySkillEntry *git_entry = NULL;
    for (size_t i = 0; i < git_entries.count; i++) {
        ClaySkillEntry *candidate = clay_array_get(&git_entries, i);
        if (strcmp(candidate->name, "git-skill") == 0) git_entry = candidate;
    }
    assert(git_entry);
    ClayStr git_marker;
    clay_str_init(&git_marker);
    clay_str_printf(&git_marker, "%s/.git/HEAD", git_entry->path);
    FILE *marker = fopen(git_marker.data, "r");
    assert(marker);
    fclose(marker);
    clay_str_free(&git_marker);
    clay_skill_entries_free(&git_entries);

    /* Reinstalling hits the `git pull` path (the clone already exists). */
    assert(clay_skill_install(git_url.data, NULL) == 0);
    clay_str_free(&git_url);
    assert(setenv("HOME", home, 1) == 0);

    ClayStr path;
    clay_str_init(&path);
    clay_str_printf(&path, "%s/.clay/skills/index.json", home);
    assert(remove(path.data) == 0);
    clay_str_clear(&path);
    clay_str_printf(&path, "%s/.clay/skills", home);
    assert(rmdir(path.data) == 0);
    clay_str_clear(&path);
    clay_str_printf(&path, "%s/.clay", home);
    assert(rmdir(path.data) == 0);
    assert(rmdir(home) == 0);
    clay_str_free(&path);

    assert(remove(skill_md.data) == 0);
    assert(rmdir(source_dir) == 0);
    clay_str_free(&skill_md);
    return 0;
}
