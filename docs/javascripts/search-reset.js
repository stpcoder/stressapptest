(() => {
  const highlightSelector = ".md-content [data-md-highlight]";

  function clearPageSearch(input) {
    const url = new URL(window.location.href);
    const hasQuery = url.searchParams.has("h");
    const highlights = document.querySelectorAll(highlightSelector);

    if (!hasQuery && highlights.length === 0) {
      return;
    }

    if (input.value) {
      input.value = "";
      input.dispatchEvent(new Event("input", { bubbles: true }));
    }

    highlights.forEach((mark) => {
      mark.replaceWith(document.createTextNode(mark.textContent || ""));
    });

    url.searchParams.delete("h");
    window.history.replaceState(window.history.state, "", url);
  }

  document.addEventListener("pointerdown", (event) => {
    if (event.target instanceof Element) {
      const input = event.target.closest(".md-search__input");
      if (input instanceof HTMLInputElement) {
        clearPageSearch(input);
      }
    }
  });

  document.addEventListener("focusin", (event) => {
    if (event.target instanceof HTMLInputElement &&
        event.target.matches(".md-search__input")) {
      clearPageSearch(event.target);
    }
  });
})();
