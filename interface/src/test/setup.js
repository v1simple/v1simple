import '@testing-library/jest-dom/vitest';

// jsdom has no native dialog lifecycle. Model open state only; browser checks
// cover initial focus, Tab confinement, Escape dispatch and focus restoration.
HTMLDialogElement.prototype.showModal = function () {
    this.open = true;
};
HTMLDialogElement.prototype.close = function () {
    this.open = false;
};
